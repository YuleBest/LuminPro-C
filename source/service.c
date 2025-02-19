/*
 * 本代码实现了：
 *  1. 读取默认亮度参数（FDBRI、MAXBRI）；
 *  2. 从 CONFIG.prop 中读取可选自定义参数并更新全局参数；
 *  3. 日志记录（service.log）和 module.prop 描述更新；
 *  4. 根据设定规则检查当前屏幕亮度、时间范围，并决定是否进行亮度提升；
 *  5. 提升亮度时支持直接或分步提升；
 *  6. 日志归档和清理（归档目录 log-arch）。
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#define LOG_MAX_SIZE 100000
#define ARCHIVE_MAX_COUNT 5
#define SECONDS_IN_DAY (7*24*3600)

const char *BRI_PATH = "/sys/class/backlight/panel0-backlight/brightness";

/// 全局路径（模块目录），初始化时从 argv[0] 解析获得
char MODDIR[PATH_MAX] = {0};

/// 默认亮度参数（从 MODDIR/yule/ 文件中读取）
int default_FDBRI = 0;
int default_MAXBRI = 0;

/// 实时使用的阈值和最大亮度（可能会被 CONFIG 覆盖）
int FDBRI = 0;
int MAXBRI = 0;

/// 计算出的每步增加的步长
int FOOTSTEP = 0;

/// 状态机标志，1 表示满足条件需提升亮度，0 表示不调整
int ADJUSTMENT = 0;

/// 配置参数结构体，从 CONFIG.prop 读取
typedef struct {
    int custom_max_bri;    // 自定义最大亮度（若非0则覆盖默认 MAXBRI）
    int custom_thr_bri;    // 自定义激发亮度（若非0则覆盖默认 FDBRI）
    int boost_wait_time;   // 模块启动等待时间（默认30）
    int flash_wait_time;   // 循环刷新间隔（默认3）
    int bri_update_mode;   // 亮度提升模式：1 直接提升，2 分步提升（默认1）
    int sleep_start;       // 睡眠时间起始小时（若未设置，则为25，表示不生效）
    int sleep_stop;        // 睡眠时间结束小时（同上）
    int step_num;          // 分步提升时步数（默认10）
} Config;

Config config = {
    .custom_max_bri = 0,
    .custom_thr_bri = 0,
    .boost_wait_time = 30,
    .flash_wait_time = 3,
    .bri_update_mode = 1,
    .sleep_start = 25,
    .sleep_stop = 25,
    .step_num = 10
};

/// 获取当前时间的格式化字符串，格式为 "MM-DD HH:MM:SS"
void get_current_time_str(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    strftime(buf, len, "%m-%d %T", tm_now);
}

/// 追加日志记录到 MODDIR/service.log
void log_message(const char *level, const char *msg) {
    char time_str[32];
    get_current_time_str(time_str, sizeof(time_str));

    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/service.log", MODDIR);
    FILE *fp = fopen(log_path, "a");
    if (!fp) return;
    fprintf(fp, "%s %s : %s\n", time_str, level, msg);
    fclose(fp);
}

/// 读取文本文件中的整数值（假设文件中只有一个数字）
int read_int_from_file(const char *filepath) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) return 0;
    int value = 0;
    fscanf(fp, "%d", &value);
    fclose(fp);
    return value;
}

/// 将整数值写入文本文件（覆盖写入）
int write_int_to_file(const char *filepath, int value) {
    FILE *fp = fopen(filepath, "w");
    if (!fp) return -1;
    fprintf(fp, "%d", value);
    fclose(fp);
    return 0;
}

/// 解析 MODDIR/CONFIG.prop，更新 config 结构体
void load_config() {
    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/CONFIG.prop", MODDIR);
    
    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        log_message("W", "无法打开 CONFIG.prop，使用默认配置");
        return;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        // 忽略注释和空行
        if (line[0]=='#' || line[0]=='\n') continue;
        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
            if (strcmp(key, "custom_max_bri") == 0) {
                config.custom_max_bri = atoi(val);
            } else if (strcmp(key, "custom_thr_bri") == 0) {
                config.custom_thr_bri = atoi(val);
            } else if (strcmp(key, "boost_wait_time") == 0) {
                config.boost_wait_time = atoi(val) > 0 ? atoi(val) : 30;
            } else if (strcmp(key, "flash_wait_time") == 0) {
                config.flash_wait_time = atoi(val) > 0 ? atoi(val) : 3;
            } else if (strcmp(key, "bri_update_mode") == 0) {
                int mode = atoi(val);
                if (mode == 1 || mode == 2)
                    config.bri_update_mode = mode;
                else {
                    log_message("W", "bri_update_mode 参数无效，已重置为 1");
                    config.bri_update_mode = 1;
                }
            } else if (strcmp(key, "sleep_start") == 0) {
                config.sleep_start = atoi(val);
            } else if (strcmp(key, "sleep_stop") == 0) {
                config.sleep_stop = atoi(val);
            } else if (strcmp(key, "step_num") == 0) {
                config.step_num = atoi(val) > 0 ? atoi(val) : 10;
            }
        }
    }
    fclose(fp);
}

/// 根据 CONFIG.prop 更新 FDBRI 和 MAXBRI（若自定义参数有效则覆盖默认值）
void CONFIG_UPDATE() {
    load_config();
    
    char buf[256];
    if (config.custom_max_bri == 0) {
        snprintf(buf, sizeof(buf), "检测到 custom_max_bri 为 0 或空，这次循环用的是默认值 %d", MAXBRI);
        log_message("I", buf);
    } else {
        snprintf(buf, sizeof(buf), "检测到 custom_max_bri 为 %d，这次循环用的是自定义值哦", config.custom_max_bri);
        log_message("I", buf);
        MAXBRI = config.custom_max_bri;
    }
    
    if (config.custom_thr_bri == 0) {
        snprintf(buf, sizeof(buf), "检测到 custom_thr_bri 为 0 或空，这次循环用的是默认值 %d", FDBRI);
        log_message("I", buf);
    } else {
        snprintf(buf, sizeof(buf), "检测到 custom_thr_bri 为 %d，这次循环用的是自定义值哦", config.custom_thr_bri);
        log_message("I", buf);
        FDBRI = config.custom_thr_bri;
    }
    // boost_wait_time 和 flash_wait_time 已在 load_config 中设置默认值
}

/// 计算提升步长 FOOTSTEP = (MAXBRI - FDBRI) / step_num
void UPDATE_CALCULATION() {
    if (config.step_num <= 0) config.step_num = 10;
    FOOTSTEP = (MAXBRI - FDBRI) / config.step_num;
    
    char buf[128];
    snprintf(buf, sizeof(buf), "调整步长 FOOTSTEP 为 %d", FOOTSTEP);
    log_message("I", buf);
}

/// 更新 module.prop 中 description 字段
void dec_up(const char *desc) {
    char modprop_path[PATH_MAX];
    snprintf(modprop_path, sizeof(modprop_path), "%s/module.prop", MODDIR);
    
    // 读入所有行
    FILE *fp = fopen(modprop_path, "r");
    if (!fp) return;
    char **lines = NULL;
    size_t count = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        lines = realloc(lines, (count + 1) * sizeof(char *));
        lines[count] = strdup(line);
        count++;
    }
    fclose(fp);
    
    // 去掉最后一行（假设为旧的 description）
    if (count > 0) {
        free(lines[count - 1]);
        count--;
    }
    
    // 重新写入 module.prop
    fp = fopen(modprop_path, "w");
    if (!fp) {
        for (size_t i = 0; i < count; i++) free(lines[i]);
        free(lines);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        fputs(lines[i], fp);
        free(lines[i]);
    }
    free(lines);
    
    fprintf(fp, "description=%s", desc);
    fclose(fp);
}

/// 检查当前亮度和时间规则，设置 ADJUSTMENT 标志
void BRI_CHECK() {
    int now_bri = read_int_from_file(BRI_PATH);
    int dif_bri = FDBRI - now_bri;
    int current_hour = 0;
    {
        time_t now = time(NULL);
        struct tm *tm_now = localtime(&now);
        current_hour = tm_now->tm_hour;
    }
    
    // 条件：dif_bri <= 0 且 now_bri <= MAXBRI*9/10 且 now_bri > 0
    if (dif_bri <= 0 && now_bri <= (MAXBRI * 9 / 10) && now_bri > 0) {
        // 如果未设置睡眠规则，则直接调整
        if (config.sleep_start == 25 && config.sleep_stop == 25) {
            log_message("I", "时间规则未设置，不进行时间判断");
            ADJUSTMENT = 1;
        } else {
            // 若当前小时在睡眠区间内，则不调整；否则调整
            if (current_hour >= config.sleep_start && current_hour <= config.sleep_stop) {
                ADJUSTMENT = 0;
            } else {
                ADJUSTMENT = 1;
            }
        }
    } else {
        ADJUSTMENT = 0;
    }
    
    char buf[256];
    snprintf(buf, sizeof(buf), "NOWBRI 为 %d，DIFBRI 为 %d，当前小时 %d，休眠规则为 %d 到 %d，ADJUSTMENT 状态机设定为 %d",
             now_bri, dif_bri, current_hour, config.sleep_start, config.sleep_stop, ADJUSTMENT);
    log_message("I", buf);
}

/// 根据配置进行亮度提升
void BRI_UPDATE() {
    int now_bri = FDBRI;
    char buf[256];
    if (config.bri_update_mode == 1) {
        snprintf(buf, sizeof(buf), "bri_update_mode 为 1，亮度从 %d 提升到 %d", FDBRI, MAXBRI);
        log_message("GO", buf);
    } else if (config.bri_update_mode == 2) {
        snprintf(buf, sizeof(buf), "bri_update_mode 为 2，亮度分 %d 步从 %d 提升到 %d", config.step_num, FDBRI, MAXBRI);
        log_message("GO", buf);
        for (int step = 1; step <= config.step_num; step++) {
            now_bri += FOOTSTEP;
            write_int_to_file(BRI_PATH, now_bri);
            // 若需要更平滑的体验，可以在此处增加短暂 sleep
        }
    }
    // 最后确保设置到 MAXBRI
    write_int_to_file(BRI_PATH, MAXBRI);
}

/// 日志清理：归档 service.log 超大文件，并清理过旧日志
void log_cleaner() {
    char log_path[PATH_MAX], archive_dir[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/service.log", MODDIR);
    snprintf(archive_dir, sizeof(archive_dir), "%s/log-arch", MODDIR);
    
    // 确保归档目录存在
    struct stat st;
    if (stat(archive_dir, &st) != 0) {
        mkdir(archive_dir, 0755);
    }
    
    // 获取 service.log 大小
    if (stat(log_path, &st) == 0) {
        if (st.st_size > LOG_MAX_SIZE) {
            char archive_name[PATH_MAX];
            time_t now = time(NULL);
            struct tm *tm_now = localtime(&now);
            char timebuf[32];
            strftime(timebuf, sizeof(timebuf), "%Y%m%d-%H%M%S", tm_now);
            snprintf(archive_name, sizeof(archive_name), "%s/log-%s.log", archive_dir, timebuf);
            if (rename(log_path, archive_name) == 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "日志已归档为 %s", archive_name);
                log_message("I", msg);
            }
        }
    }
    
    // 统计归档日志数量，并删除最旧的（若超过 ARCHIVE_MAX_COUNT）
    int count = 0;
    DIR *dir = opendir(archive_dir);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG)
                count++;
        }
        closedir(dir);
    }
    if (count > ARCHIVE_MAX_COUNT) {
        // 遍历目录，找到最旧的文件
        dir = opendir(archive_dir);
        if (dir) {
            time_t oldest = time(NULL);
            char oldest_path[PATH_MAX] = {0};
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_type == DT_REG) {
                    char filepath[PATH_MAX];
                    snprintf(filepath, sizeof(filepath), "%s/%s", archive_dir, entry->d_name);
                    struct stat filestat;
                    if (stat(filepath, &filestat) == 0) {
                        if (filestat.st_mtime < oldest) {
                            oldest = filestat.st_mtime;
                            strncpy(oldest_path, filepath, sizeof(oldest_path)-1);
                        }
                    }
                }
            }
            closedir(dir);
            if (oldest_path[0] != '\0') {
                remove(oldest_path);
                char msg[256];
                snprintf(msg, sizeof(msg), "删除了最旧的归档日志 %s", oldest_path);
                log_message("I", msg);
            }
        }
    }
    
    // 删除 7 天前的日志
    dir = opendir(archive_dir);
    if (dir) {
        struct dirent *entry;
        time_t now = time(NULL);
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                char filepath[PATH_MAX];
                snprintf(filepath, sizeof(filepath), "%s/%s", archive_dir, entry->d_name);
                struct stat filestat;
                if (stat(filepath, &filestat) == 0) {
                    if (difftime(now, filestat.st_mtime) > SECONDS_IN_DAY) {
                        remove(filepath);
                    }
                }
            }
        }
        closedir(dir);
    }
}

/// 从 argv0 解析出目录部分赋给 MODDIR
void init_moddir(const char *argv0) {
    strncpy(MODDIR, argv0, sizeof(MODDIR)-1);
    char *last_slash = strrchr(MODDIR, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        // 如果没有斜杠，则使用当前目录
        strcpy(MODDIR, ".");
    }
}

int main(int argc, char *argv[]) {
    // 初始化模块目录
    if (argc > 0) {
        init_moddir(argv[0]);
    } else {
        strcpy(MODDIR, ".");
    }
    
    // 读取默认亮度参数（例如存放在 MODDIR/yule/FDBRI 和 MAXBRI 中）
    char path_buf[PATH_MAX];
    snprintf(path_buf, sizeof(path_buf), "%s/yule/FDBRI", MODDIR);
    default_FDBRI = read_int_from_file(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/yule/MAXBRI", MODDIR);
    default_MAXBRI = read_int_from_file(path_buf);
    
    // 初始化全局 FDBRI、MAXBRI
    FDBRI = default_FDBRI;
    MAXBRI = default_MAXBRI;
    
    // 首次载入配置
    CONFIG_UPDATE();
    
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "模块启动成功，将在 %d 秒后开始主循环喵", config.boost_wait_time);
        log_message("I", msg);
    }
    sleep(config.boost_wait_time);
    
    // 主循环
    while (1) {
        // 检查是否存在 MODDIR/DONT-RUN 文件，若存在则跳过循环
        char dont_run[PATH_MAX];
        snprintf(dont_run, sizeof(dont_run), "%s/DONT-RUN", MODDIR);
        if (access(dont_run, F_OK) != 0) {
            // 更新 module.prop 的描述（包含当前亮度、规则、刷新间隔）
            int current_bri = read_int_from_file(BRI_PATH);
            char desc[256];
            snprintf(desc, sizeof(desc), "模块状态 正常  当前亮度 %d  提升规则 %d → %d  刷新间隔 %d 秒",
                     current_bri, FDBRI, MAXBRI, config.flash_wait_time);
            dec_up(desc);
            
            // 每次循环重新载入配置和计算步长
            CONFIG_UPDATE();
            UPDATE_CALCULATION();
            ADJUSTMENT = 0;
            BRI_CHECK();
            log_cleaner();
            
            if (ADJUSTMENT == 1) {
                log_message("START", "满足条件，开始调整亮度");
                BRI_UPDATE();
                log_message("STOP", "亮度调整完毕");
            } else {
                log_message("SKIP", "不满足条件，等待下次循环");
            }
            char over_msg[128];
            snprintf(over_msg, sizeof(over_msg), "循环结束，%d 秒后开启下一次循环~", config.flash_wait_time);
            log_message("OVER", over_msg);
        }
        sleep(config.flash_wait_time);
    }
    
    return 0;
}