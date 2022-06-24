/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include "ssd_fuse_header.h"
#include <errno.h>
#include <fuse.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define SSD_NAME "ssd_file"

/*
前半是block(nand)，>>16之後*10(PAGE_PER_BLOCK)
+這個bloack的lba就4index
*/
#define PCA_IDX(pca) ((pca & 0xffff) + ((pca >> 16) * PAGE_PER_BLOCK))

void gomi_atsumeru();

enum {
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};

static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

typedef union pca_rule PCA_RULE;
union pca_rule {
    unsigned int pca;
    struct
    {
        unsigned int lba : 16;
        unsigned int nand : 16;
    } fields;
};

PCA_RULE curr_pca;
static unsigned int get_next_pca();

unsigned int *L2P, *P2L, *valid_count, *pca_state, free_block_number;

static int ssd_resize(size_t new_size) {
    // set logic size to new_size
    if (new_size > NAND_SIZE_KB * 1024) {
        return -ENOMEM;
    } else {
        logic_size = new_size;
        return 0;
    }
}

static int ssd_expand(size_t new_size) {
    // logic must less logic limit

    if (new_size > logic_size) {
        return ssd_resize(new_size);
    }

    return 0;
}

static int nand_read(char *buf, int pca) {
    char nand_name[100];
    FILE *fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.nand);

    // read
    if ((fptr = fopen(nand_name, "r"))) {
        fseek(fptr, my_pca.fields.lba * 512, SEEK_SET);
        fread(buf, 1, 512, fptr);
        fclose(fptr);
    } else {
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    return 512;
}
static int nand_write(const char *buf, int pca) {
    char nand_name[100];
    FILE *fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.nand);

    // write
    if ((fptr = fopen(nand_name, "r+"))) {
        /// 已經知道在nand的哪個blockㄌ，從他的page開始拿一個page
        fseek(fptr, my_pca.fields.lba * 512, SEEK_SET);
        fwrite(buf, 1, 512, fptr);
        fclose(fptr);
        physic_size++;
        valid_count[my_pca.fields.nand]++;
    } else {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }

    nand_write_size += 512;
    return 512;
}

static int nand_erase(int block_index) {
    char nand_name[100];
    FILE *fptr;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, block_index);
    fptr = fopen(nand_name, "w");
    if (fptr == NULL) {
        printf("erase nand_%d fail", block_index);
        return 0;
    }
    fclose(fptr);
    valid_count[block_index] = FREE_BLOCK;
    return 1;
}

static unsigned int get_next_block() {
    for (int i = 0; i < PHYSICAL_NAND_NUM; i++) {
        if (valid_count[(curr_pca.fields.nand + i) % PHYSICAL_NAND_NUM] == FREE_BLOCK) {
            curr_pca.fields.nand = (curr_pca.fields.nand + i) % PHYSICAL_NAND_NUM;
            curr_pca.fields.lba = 0;
            free_block_number--;
            valid_count[curr_pca.fields.nand] = 0;
            /// 檢查現在拿到的已經是最後一塊空ㄉ
            /// 該GCㄌ
            if (free_block_number == 0) {
                gomi_atsumeru();
            }
            return curr_pca.pca;
        }
    }
    return OUT_OF_BLOCK;
}
static unsigned int get_next_pca() {
    if (curr_pca.pca == INVALID_PCA) {
        // init
        curr_pca.pca = 0;
        valid_count[0] = 0;
        free_block_number--;
        return curr_pca.pca;
    }

    if (curr_pca.fields.lba == 9) {
        int temp = get_next_block();
        if (temp == OUT_OF_BLOCK) {
            return OUT_OF_BLOCK;
        } else if (temp == -EINVAL) {
            return -EINVAL;
        } else {
            return temp;
        }
    } else {
        curr_pca.fields.lba += 1;
    }
    return curr_pca.pca;
}

static int ftl_read(char *buf, size_t lba) {
    // TODO done
    /// 一開始先找轉址，看有沒有這一塊
    PCA_RULE rule;
    rule.pca = L2P[lba];
    if (rule.pca == INVALID_PCA) {
        /// 找不到資料的區塊
        perror("[WARNING] READ OUT OF BOUND!\n");
        /// 不確定要不要重設0
        memset(buf, 0, 512);
        return 512;
    }
    return nand_read(buf, rule.pca);
}

static int ftl_write(const char *buf, size_t lba_rnage, size_t lba) {
    // TODO
    int ret;
    unsigned int pca;
    int check = get_next_pca();
    /// 如果拿不到下一塊
    if (curr_pca.fields.lba == OUT_OF_BLOCK || check == -ENAVAIL) {
        perror("[ERROR] CAN'T get_next_pca();\n");
        return 0;
    }
    /// 舊ㄉ資料不要ㄌ，PCA要設為無效，然後把valid_count減少1
    if (L2P[lba] != INVALID_PCA) {
        P2L[PCA_IDX(L2P[lba])] = INVALID_LBA;
        valid_count[((PCA_RULE)L2P[lba]).fields.nand]--;
    }
    ret = nand_write(buf, curr_pca.pca);
    L2P[lba] = curr_pca.pca;
    /// 紀錄phys對應到的lba
    P2L[PCA_IDX(curr_pca.pca)] = lba;
    return ret;
}

static int ssd_file_type(const char *path) {
    if (strcmp(path, "/") == 0) {
        return SSD_ROOT;
    }
    if (strcmp(path, "/" SSD_NAME) == 0) {
        return SSD_FILE;
    }
    return SSD_NONE;
}
static int ssd_getattr(const char *path, struct stat *stbuf,
                       struct fuse_file_info *fi) {
    (void)fi;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = stbuf->st_mtime = time(NULL);
    switch (ssd_file_type(path)) {
    case SSD_ROOT:
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        break;
    case SSD_FILE:
        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_nlink = 1;
        stbuf->st_size = logic_size;
        break;
    case SSD_NONE:
        return -ENOENT;
    }
    return 0;
}
static int ssd_open(const char *path, struct fuse_file_info *fi) {
    (void)fi;
    if (ssd_file_type(path) != SSD_NONE) {
        return 0;
    }
    return -ENOENT;
}
static int ssd_do_read(char *buf, size_t size, off_t offset) {
    int tmp_lba, tmp_lba_range, rst;
    char *tmp_buf;

    // off limit
    if ((offset) >= logic_size) {
        return 0;
    }
    if (size > logic_size - offset) {
        // is valid data section
        size = logic_size - offset;
    }

    /// 起始位置是從第幾個 page開始
    tmp_lba = offset / 512;
    /// 要讀幾次
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;
    /// calloc，需共讀出幾個page的大小
    tmp_buf = calloc(tmp_lba_range * 512, sizeof(char));

    for (int i = 0; i < tmp_lba_range; i++) {
        // TODO done
        /// 從offset的那塊page開時讀到tmp_buf的偏移位置上
        ftl_read(tmp_buf + i * 512, tmp_lba + i);
    }
    /// 因為一次就是讀取512byte，複製的時候要從第一塊的offset那邊開始傳回去
    memcpy(buf, tmp_buf + offset % 512, size);

    printf("read_offset: %d\n", offset % 512);

    free(tmp_buf);
    return size;
}
static int ssd_read(const char *path, char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi) {
    (void)fi;
    if (ssd_file_type(path) != SSD_FILE) {
        return -EINVAL;
    }
    return ssd_do_read(buf, size, offset);
}
static int ssd_do_write(const char *buf, size_t size, off_t offset) {
    int tmp_lba, tmp_lba_range, process_size;
    int idx, curr_size, remain_size, rst;
    char *tmp_buf;

    host_write_size += size;
    if (ssd_expand(offset + size) != 0) {
        return -ENOMEM;
    }

    /// 跟read一樣先看是要寫到哪個block
    tmp_lba = offset / 512;
    /// 算總共寫了幾個block
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;

    process_size = 0;
    remain_size = size;
    curr_size = 0;
    tmp_buf = calloc(512, sizeof(char));
    for (idx = 0; idx < tmp_lba_range; idx++) {
        // TODO
        int start = 0, end = 512;
        /// 需特別處理第一塊，因為寫是一次寫整塊，要把原本的資料也抓出來
        /// 另外，如果一開始的offset就是該塊的開頭，不用特別處理
        if (idx == 0 && (offset % 512 != 0)) {
            /// 第一塊的時候不用memset，因為一開始就用calloc來allocate
            ftl_read(tmp_buf, tmp_lba + idx);
            start = offset % 512;
        }
        /// 如果是最後一塊，看這次write寫完時沒有剛好一個block，就要特別處理
        /// 阿如果最後一塊要寫整塊，也不用特別讀了
        else if (idx == (tmp_lba_range - 1) && ((offset + size) % 512) != 0) {
            ftl_read(tmp_buf, tmp_lba + idx);
            end = (offset + size) % 512;
        }
        /// 第一塊時，可能會把讀到的資料的後半寫上新的
        /// 最後一塊，可能把tmp_buf的前半蓋成這次要寫的
        /// 不用顧慮是否要把頭尾串回來的話，直接寫整塊
        for (int i = start; i < end; i++) {
            tmp_buf[i] = buf[process_size++];
        }
        ftl_write(tmp_buf, 512, tmp_lba + idx);
        /// 每次結束都先把tmp_buf清空
        memset(tmp_buf, 0, 512 * sizeof(char));
        printf("write_offset: %d\n", start);
    }
    return size;
}
static int ssd_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi) {

    (void)fi;
    if (ssd_file_type(path) != SSD_FILE) {
        return -EINVAL;
    }
    return ssd_do_write(buf, size, offset);
}
static int ssd_truncate(const char *path, off_t size,
                        struct fuse_file_info *fi) {
    (void)fi;
    memset(L2P, INVALID_PCA, sizeof(int) * LOGICAL_NAND_NUM * PAGE_PER_BLOCK);
    memset(P2L, INVALID_LBA, sizeof(int) * PHYSICAL_NAND_NUM * PAGE_PER_BLOCK);
    memset(valid_count, FREE_BLOCK, sizeof(int) * PHYSICAL_NAND_NUM);
    curr_pca.pca = INVALID_PCA;
    free_block_number = PHYSICAL_NAND_NUM;
    if (ssd_file_type(path) != SSD_FILE) {
        return -EINVAL;
    }

    return ssd_resize(size);
}
static int ssd_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags) {
    (void)fi;
    (void)offset;
    (void)flags;
    if (ssd_file_type(path) != SSD_ROOT) {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, SSD_NAME, NULL, 0, 0);
    return 0;
}
static int ssd_ioctl(const char *path, unsigned int cmd, void *arg,
                     struct fuse_file_info *fi, unsigned int flags, void *data) {

    if (ssd_file_type(path) != SSD_FILE) {
        return -EINVAL;
    }
    if (flags & FUSE_IOCTL_COMPAT) {
        return -ENOSYS;
    }
    switch (cmd) {
    case SSD_GET_LOGIC_SIZE:
        *(size_t *)data = logic_size;
        return 0;
    case SSD_GET_PHYSIC_SIZE:
        *(size_t *)data = physic_size;
        return 0;
    case SSD_GET_WA:
        *(double *)data = (double)nand_write_size / (double)host_write_size;
        return 0;
    }
    return -EINVAL;
}

void gomi_atsumeru() {
    unsigned int less_nand = (1 + curr_pca.fields.nand) % PHYSICAL_NAND_NUM;
    /// 找最空的出來
    for (int i = 1; i < PHYSICAL_NAND_NUM; i++) {
        unsigned int idx = (i + curr_pca.fields.nand) % PHYSICAL_NAND_NUM;
        if (idx == curr_pca.fields.nand)
            continue;
        if (valid_count[idx] < valid_count[less_nand]) {
            less_nand = idx;
        }
    }
    PCA_RULE less_pca;
    less_pca.fields.nand = less_nand;
    less_pca.fields.lba = 0;
    char *buf[512];
    /// 然後就把那個block上有用的資料都搬出來
    for (int i = 0; i < PAGE_PER_BLOCK; i++) {
        less_pca.fields.lba = i;
        /// 取出p2l的index
        int idx = PCA_IDX(less_pca.pca);
        unsigned int lba = P2L[idx];
        if (P2L[idx] == INVALID_LBA)
            continue;
        nand_read(buf, less_pca.pca);
        /// 把資料寫到當前pca
        nand_write(buf, curr_pca.pca);
        L2P[lba] = curr_pca.pca;
        P2L[PCA_IDX(curr_pca.pca)] = lba;
        P2L[idx] = INVALID_LBA;
        curr_pca.fields.lba++;
    }
    nand_erase(less_nand);
    free_block_number++;
}

static const struct fuse_operations ssd_oper =
    {
        .getattr = ssd_getattr,
        .readdir = ssd_readdir,
        .truncate = ssd_truncate,
        .open = ssd_open,
        .read = ssd_read,
        .write = ssd_write,
        .ioctl = ssd_ioctl,
};
int main(int argc, char *argv[]) {
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
    curr_pca.pca = INVALID_PCA;
    free_block_number = PHYSICAL_NAND_NUM;

    L2P = malloc(LOGICAL_NAND_NUM * PAGE_PER_BLOCK * sizeof(int));
    /// memset不是看一byteㄇ，怎麼一次填4byte
    memset(L2P, INVALID_PCA, sizeof(int) * LOGICAL_NAND_NUM * PAGE_PER_BLOCK);
    P2L = malloc(PHYSICAL_NAND_NUM * PAGE_PER_BLOCK * sizeof(int));
    /// memset不是看一byteㄇ，怎麼一次填4byte
    memset(P2L, INVALID_LBA, sizeof(int) * PHYSICAL_NAND_NUM * PAGE_PER_BLOCK);
    valid_count = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    memset(valid_count, FREE_BLOCK, sizeof(int) * PHYSICAL_NAND_NUM);

    pca_state = malloc(LOGICAL_NAND_NUM * PAGE_PER_BLOCK * sizeof(int));
    memset(pca_state, 0xFF, sizeof(int) * LOGICAL_NAND_NUM * PAGE_PER_BLOCK);

    // create nand file
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++) {
        FILE *fptr;
        snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
        fptr = fopen(nand_name, "w");
        if (fptr == NULL) {
            printf("open fail");
        }
        fclose(fptr);
    }
    return fuse_main(argc, argv, &ssd_oper, NULL);
}
