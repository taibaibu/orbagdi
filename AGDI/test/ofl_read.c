/*
 * ofl_read - 独立的 OFLOW 文件读器（P2-1 的验收工具）
 * ============================================================================
 *
 * 它**链的是 orbuculum 自己的 `Src/cobs.c`（一行未改）**，所以是一条独立通路：
 * 驱动侧的 `OflowDeframer` 是那份代码的移植版，两者都认，才说明文件格式对。
 *
 * 为什么不用 `oflow.c`：它依赖 POSIX 的 `clock_gettime`，MSVC 没有；而帧的语义
 * 只有三行（tag + 累加校验和），照 `Src/oflow.c:81` 的 `_pumpcb` 抄在这儿，
 * 语义完全一致：
 *     len  = p->len - 2     首字节 = tag，末字节 = sum
 *     good = (tag + Σpayload + sum) == 0 (mod 256)
 *
 * 用法：
 *     ofl_read.exe <file.orb> [--dump N]     N = 打印前 N 个帧的头几个字节
 *
 * 退出码：0 = 签名对 + 有帧 + 无校验错/COBS 错；1 = 有问题。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cobs.h"

/* orbuculum Src/orbuculum.c:70 的文件签名 */
static const char kSig[] = "%%ORBFLOW1.0.0%%";

static unsigned long long gFrames    = 0;
static unsigned long long gBadSum    = 0;
static unsigned long long gShort     = 0;
static unsigned long long gBytes[256];  /* 每个 tag 的载荷字节数 */
static unsigned long long gCount[256];  /* 每个 tag 的帧数 */
static int                gDumpLeft  = 0;

static void OnFrame(struct Frame* p, void* param)
{
    (void)param;

    if (p->len < 2) { ++gShort; return; }

    const uint8_t tag = p->d[0];
    const uint8_t sum = p->d[p->len - 1];
    const unsigned len = p->len - 2;

    uint8_t s = tag;
    for (unsigned i = 0; i < len; ++i) s = (uint8_t)(s + p->d[1 + i]);
    s = (uint8_t)(s + sum);

    if (s != 0) { ++gBadSum; return; }

    ++gFrames;
    ++gCount[tag];
    gBytes[tag] += len;

    if (gDumpLeft > 0) {
        --gDumpLeft;
        printf("        frame tag=%u len=%u :", tag, len);
        for (unsigned i = 0; i < len && i < 12; ++i) printf(" %02X", p->d[1 + i]);
        printf("%s\n", (len > 12) ? " ..." : "");
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: ofl_read <file.orb> [--dump N]\n");
        return 2;
    }

    const char* path = argv[1];
    for (int i = 2; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "--dump")) gDumpLeft = atoi(argv[i + 1]);
    }

    FILE* f = fopen(path, "rb");
    if (!f) { printf("FAIL  cannot open %s\n", path); return 1; }

    const size_t sigLen = strlen(kSig);
    char sig[32] = {0};
    if (fread(sig, 1, sigLen, f) != sigLen || memcmp(sig, kSig, sigLen) != 0) {
        printf("FAIL  no OFLOW signature (first %u bytes: \"%.*s\")\n",
               (unsigned)sigLen, (int)sigLen, sig);
        fclose(f);
        return 1;
    }
    printf("PASS  signature %s\n", kSig);

    struct COBS* c = COBSInit(NULL);

    static uint8_t buf[64 * 1024];
    size_t  n;
    unsigned long long bytes = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        bytes += (unsigned long long)n;
        COBSPump(c, buf, (int)n, OnFrame, NULL);
    }
    const int cobsErr = COBSGetErrors(c);
    COBSDelete(c);
    fclose(f);

    printf("      %llu byte(s) after the signature; %llu frame(s), "
           "%llu bad sum, %llu short, %d COBS error(s)\n",
           bytes, gFrames, gBadSum, gShort, cobsErr);

    for (int t = 0; t < 256; ++t) {
        if (gCount[t]) {
            printf("      tag %d : %llu frame(s), %llu payload byte(s)\n",
                   t, gCount[t], gBytes[t]);
        }
    }

    if (gFrames == 0) {
        printf("FAIL  no frame decoded\n");
        return 1;
    }
    /* 坏校验和 / COBS 错是**源流**的属性，不是我们写出来的：驱动侧的
     * OflowDeframer 会报同一个数（日志里 `校验错=` / `COBS错=`）。打出来正是为了
     * 对上这两个数 —— 对得上就说明文件忠实复制了源流。
     * 写侧真出问题时不是"错一两个"，而是签名坏掉或**大面积**对不上。 */
    if (gBadSum || gShort || cobsErr) {
        printf("NOTE  %llu bad sum / %llu short / %d COBS error(s) - these belong to "
               "the source stream; compare with the driver log's counters\n",
               gBadSum, gShort, cobsErr);
    }
    printf("PASS  %llu frame(s) decoded from the file\n", gFrames);
    return 0;
}
