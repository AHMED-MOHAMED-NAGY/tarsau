#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>

#define MAX_INPUT_FILES     32
#define MAX_TOTAL_SIZE      (200UL * 1024UL * 1024UL)
#define HEADER_SIZE         10
#define IO_BUFFER_SIZE      8192

typedef struct {
    char filepath[PATH_MAX];
    char basename_str[256];
    mode_t permissions;
    off_t  size;
} ArchiveEntry;

static void        print_usage(void);
static const char *extract_basename(const char *path);
static int         is_ascii_text_file(const char *filepath);
static int         mkdir_recursive(const char *path, mode_t mode);
static int         do_merge(int file_count, char *files[], const char *output_name);
static int         do_extract(const char *archive_path, const char *dir_name);

static void print_usage(void)
{
    fprintf(stderr,
        "Kullanım:\n"
        "  tarsau -b dosya1 dosya2 ... [-o çıktı.sau]   (Birleştirme)\n"
        "  tarsau -a arşiv.sau [dizin]                   (Açma)\n");
}

static const char *extract_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int is_ascii_text_file(const char *filepath)
{
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    unsigned char buffer[IO_BUFFER_SIZE];
    ssize_t bytes_read;

    while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
        for (ssize_t i = 0; i < bytes_read; i++) {
            if (buffer[i] > 127) {
                close(fd);
                return 0;
            }
        }
    }

    close(fd);
    return 1;
}

static int mkdir_recursive(const char *path, mode_t mode)
{
    char   tmp[PATH_MAX];
    char  *p;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);

    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

/* ======================== BIRLESTIRME (-b) ======================== */
static int do_merge(int file_count, char *files[], const char *output_name)
{
    ArchiveEntry entries[MAX_INPUT_FILES];
    off_t total_data_size = 0;

    /* Adim 1: Dosyalari dogrula ve bilgileri topla */
    for (int i = 0; i < file_count; i++) {
        struct stat st;

        if (stat(files[i], &st) != 0) {
            fprintf(stderr, "%s dosyası bulunamadı veya erişilemiyor!\n", files[i]);
            return 1;
        }

        if (!S_ISREG(st.st_mode)) {
            printf("%s giriş dosyasının formatı uyumsuzdur!\n",
                   extract_basename(files[i]));
            return 0;
        }

        if (!is_ascii_text_file(files[i])) {
            printf("%s giriş dosyasının formatı uyumsuzdur!\n",
                   extract_basename(files[i]));
            return 0;
        }

        strncpy(entries[i].filepath, files[i], PATH_MAX - 1);
        entries[i].filepath[PATH_MAX - 1] = '\0';

        strncpy(entries[i].basename_str,
                extract_basename(files[i]), sizeof(entries[i].basename_str) - 1);
        entries[i].basename_str[sizeof(entries[i].basename_str) - 1] = '\0';

        entries[i].permissions = st.st_mode & 0777;
        entries[i].size        = st.st_size;

        total_data_size += st.st_size;
    }

    /* Adim 2: 200 MB sinirini kontrol et */
    if (total_data_size > (off_t)MAX_TOTAL_SIZE) {
        fprintf(stderr, "Hata: Toplam dosya boyutu 200 MB sınırını aşıyor!\n");
        return 1;
    }

    /* Adim 3: Metadata kayitlarini olustur */
    char metadata[MAX_INPUT_FILES * (PATH_MAX + 64)];
    int  meta_offset = 0;

    for (int i = 0; i < file_count; i++) {
        int written = snprintf(metadata + meta_offset,
                               sizeof(metadata) - (size_t)meta_offset,
                               "|%s,%04o,%lld|",
                               entries[i].basename_str,
                               (unsigned int)entries[i].permissions,
                               (long long)entries[i].size);
        if (written < 0 || (size_t)written >= sizeof(metadata) - (size_t)meta_offset) {
            fprintf(stderr, "Hata: Metadata tamponu yetersiz!\n");
            return 1;
        }
        meta_offset += written;
    }

    size_t records_len     = (size_t)meta_offset;
    size_t total_meta_size = HEADER_SIZE + records_len;

    /* Adim 4: Arsiv dosyasini yaz */
    int out_fd = open(output_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        perror("Arşiv dosyası oluşturulamadı");
        return 1;
    }

    char header[HEADER_SIZE + 1];
    snprintf(header, sizeof(header), "%010zu", total_meta_size);

    if (write(out_fd, header, HEADER_SIZE) != HEADER_SIZE) {
        perror("Başlık yazma hatası");
        close(out_fd);
        return 1;
    }

    if (write(out_fd, metadata, records_len) != (ssize_t)records_len) {
        perror("Metadata yazma hatası");
        close(out_fd);
        return 1;
    }

    for (int i = 0; i < file_count; i++) {
        int in_fd = open(entries[i].filepath, O_RDONLY);
        if (in_fd < 0) {
            perror(entries[i].filepath);
            close(out_fd);
            return 1;
        }

        unsigned char buf[IO_BUFFER_SIZE];
        ssize_t n;
        while ((n = read(in_fd, buf, sizeof(buf))) > 0) {
            ssize_t total_written = 0;
            while (total_written < n) {
                ssize_t w = write(out_fd, buf + total_written, (size_t)(n - total_written));
                if (w < 0) {
                    perror("Dosya içeriği yazma hatası");
                    close(in_fd);
                    close(out_fd);
                    return 1;
                }
                total_written += w;
            }
        }

        if (n < 0) {
            perror(entries[i].filepath);
            close(in_fd);
            close(out_fd);
            return 1;
        }

        close(in_fd);
    }

    close(out_fd);

    printf("Dosyalar birleştirildi.\n");
    return 0;
}