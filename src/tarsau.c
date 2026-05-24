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


int main(int argc, char *argv[])
{
    if (argc < 3) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "-b") == 0) {

        char *input_files[MAX_INPUT_FILES];
        int   file_count  = 0;
        const char *output_name = "a.sau";

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0) {
                if (i + 1 < argc) {
                    output_name = argv[++i];
                } else {
                    fprintf(stderr, "Hata: -o parametresinden sonra dosya adı belirtilmedi.\n");
                    return 1;
                }
            } else {
                if (file_count >= MAX_INPUT_FILES) {
                    fprintf(stderr, "Hata: En fazla %d giriş dosyası desteklenir.\n",
                            MAX_INPUT_FILES);
                    return 1;
                }
                input_files[file_count++] = argv[i];
            }
        }

        if (file_count == 0) {
            fprintf(stderr, "Hata: Birleştirilecek dosya belirtilmedi.\n");
            return 1;
        }

        return do_merge(file_count, input_files, output_name);

    } else if (strcmp(argv[1], "-a") == 0) {

        if (argc > 4) {
            fprintf(stderr, "Hata: -a parametresinden sonra en fazla 2 argüman kabul edilir.\n");
            return 1;
        }

        const char *archive_path = argv[2];
        const char *dir_name     = (argc == 4) ? argv[3] : ".";

        return do_extract(archive_path, dir_name);

    } else {
        print_usage();
        return 1;
    }
}


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


/* ======================== ACMA (-a) ======================== */
static int do_extract(const char *archive_path, const char *dir_name)
{
    /* Adim 1: Arsiv dosyasini dogrula */
    size_t path_len = strlen(archive_path);
    if (path_len < 5 || strcmp(archive_path + path_len - 4, ".sau") != 0) {
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        return 0;
    }

    int arc_fd = open(archive_path, O_RDONLY);
    if (arc_fd < 0) {
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        return 0;
    }

    /* Adim 2: 10 baytlik basligi oku */
    char header[HEADER_SIZE + 1];
    ssize_t n = read(arc_fd, header, HEADER_SIZE);
    if (n != HEADER_SIZE) {
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        close(arc_fd);
        return 0;
    }
    header[HEADER_SIZE] = '\0';

    for (int i = 0; i < HEADER_SIZE; i++) {
        if (!isdigit((unsigned char)header[i])) {
            printf("Arşiv dosyası uygunsuz veya bozuk!\n");
            close(arc_fd);
            return 0;
        }
    }

    size_t total_meta_size = (size_t)atol(header);
    if (total_meta_size <= (size_t)HEADER_SIZE) {
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        close(arc_fd);
        return 0;
    }

    /* Adim 3: Metadata kayitlarini oku ve ayristir */
    size_t records_len = total_meta_size - HEADER_SIZE;
    char  *meta_buf    = malloc(records_len + 1);
    if (!meta_buf) {
        perror("Bellek ayırma hatası");
        close(arc_fd);
        return 1;
    }

    n = read(arc_fd, meta_buf, records_len);
    if (n != (ssize_t)records_len) {
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        free(meta_buf);
        close(arc_fd);
        return 0;
    }
    meta_buf[records_len] = '\0';

    ArchiveEntry entries[MAX_INPUT_FILES];
    int entry_count = 0;

    char *p = meta_buf;
    while (*p && entry_count < MAX_INPUT_FILES) {

        while (*p && *p != '|') p++;
        if (!*p) break;
        p++;

        char *end = strchr(p, '|');
        if (!end || end == p) {
            printf("Arşiv dosyası uygunsuz veya bozuk!\n");
            free(meta_buf);
            close(arc_fd);
            return 0;
        }

        size_t rec_len = (size_t)(end - p);
        char   record[PATH_MAX + 64];
        if (rec_len >= sizeof(record)) {
            printf("Arşiv dosyası uygunsuz veya bozuk!\n");
            free(meta_buf);
            close(arc_fd);
            return 0;
        }
        memcpy(record, p, rec_len);
        record[rec_len] = '\0';

        char *fname_tok = strtok(record, ",");
        char *perms_tok = strtok(NULL, ",");
        char *fsize_tok = strtok(NULL, ",");

        if (!fname_tok || !perms_tok || !fsize_tok) {
            printf("Arşiv dosyası uygunsuz veya bozuk!\n");
            free(meta_buf);
            close(arc_fd);
            return 0;
        }

        strncpy(entries[entry_count].basename_str, fname_tok,
                sizeof(entries[entry_count].basename_str) - 1);
        entries[entry_count].basename_str[sizeof(entries[entry_count].basename_str) - 1] = '\0';

        entries[entry_count].permissions = (mode_t)strtol(perms_tok, NULL, 8);
        entries[entry_count].size        = (off_t)atoll(fsize_tok);

        entry_count++;
        p = end + 1;
    }

    free(meta_buf);

    if (entry_count == 0) {
        printf("Arşiv dosyası uygunsuz veya bozuk!\n");
        close(arc_fd);
        return 0;
    }

    /* Adim 4: Cikis dizinini olustur */
    if (strcmp(dir_name, ".") != 0) {
        if (mkdir_recursive(dir_name, 0777) != 0) {
            perror("Dizin oluşturulamadı");
            close(arc_fd);
            return 1;
        }
    }

    /* Adim 5: Dosyalari cikar */
    for (int i = 0; i < entry_count; i++) {

        char out_path[PATH_MAX];
        snprintf(out_path, sizeof(out_path), "%s/%s",
                 dir_name, entries[i].basename_str);

        int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (out_fd < 0) {
            perror(out_path);
            close(arc_fd);
            return 1;
        }

        off_t remaining = entries[i].size;
        unsigned char buf[IO_BUFFER_SIZE];

        while (remaining > 0) {
            size_t  to_read    = ((size_t)remaining > IO_BUFFER_SIZE)
                                     ? IO_BUFFER_SIZE
                                     : (size_t)remaining;
            ssize_t bytes_read = read(arc_fd, buf, to_read);

            if (bytes_read <= 0) {
                printf("Arşiv dosyası uygunsuz veya bozuk!\n");
                close(out_fd);
                close(arc_fd);
                return 0;
            }

            ssize_t total_written = 0;
            while (total_written < bytes_read) {
                ssize_t w = write(out_fd, buf + total_written,
                                  (size_t)(bytes_read - total_written));
                if (w < 0) {
                    perror("Dosya yazma hatası");
                    close(out_fd);
                    close(arc_fd);
                    return 1;
                }
                total_written += w;
            }

            remaining -= bytes_read;
        }

        close(out_fd);

        if (chmod(out_path, entries[i].permissions) != 0) {
            perror("İzin ayarlama hatası");
        }
    }

    close(arc_fd);

    /* Adim 6: Basari mesajini yazdir */
    printf("%s dizininde ", dir_name);

    for (int i = 0; i < entry_count; i++) {
        printf("%s", entries[i].basename_str);

        if (i < entry_count - 2) {
            printf(", ");
        } else if (i == entry_count - 2) {
            printf(" ve ");
        }
    }

    printf(" dosyaları açıldı.\n");

    return 0;
}
