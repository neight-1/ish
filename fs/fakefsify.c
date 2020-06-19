#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <archive.h>
#include <archive_entry.h>

#define ISH_INTERNAL
#include "fs/fake.h"
#include "fs/fakefsify.h"
#include "fs/sqlutil.h"
#include "misc.h"

// I have a weird way of error handling
#define FILL_ERR(_type, _code, _message) do { \
    err_out->line = __LINE__; \
    err_out->type = _type; \
    err_out->code = _code; \
    err_out->message = strdup(_message); \
    return false; \
} while (0)
#define ARCHIVE_ERR(archive) FILL_ERR(ERR_ARCHIVE, archive_errno(archive), archive_error_string(archive))
#define POSIX_ERR() FILL_ERR(ERR_POSIX, errno, strerror(errno))
#undef HANDLE_ERR // for sqlite
#define HANDLE_ERR(db) FILL_ERR(ERR_SQLITE, sqlite3_extended_errcode(db), sqlite3_errmsg(db))

static void progress_update(struct progress *p, double progress, const char *message) {
    if (p && p->callback)
        p->callback(p->cookie, progress, message);
}

// This isn't linked with ish which is why there's so much copy/pasted code

// I hate this code
static bool path_normalize(const char *path, char *out) {
#define ends_path(c) (c == '\0' || c == '/')
    // normalized format:
    // ( '/' path-component ) *
    while (path[0] != '\0') {
        while (path[0] == '/')
            path++;
        if (path[0] == '\0')
            break; // if the path ends with a slash
        // path points to the start of a path component
        if (path[0] == '.' && path[1] == '.' && ends_path(path[2]))
            return false; // no dotdot allowed!
        if (path[0] == '.' && ends_path(path[1])) {
            path++;
        } else {
            *out++ = '/';
            while (path[0] != '/' && path[0] != '\0')
                *out++ = *path++;
        }
    }
    *out = '\0';
    return true;
}

extern const char *fix_path(const char *path);

static const char *schema = Q(
    create table meta (id integer unique default 0, db_inode integer);
    insert into meta (db_inode) values (0);
    create table stats (inode integer primary key, stat blob);
    create table paths (path blob primary key, inode integer references stats(inode));
    create index inode_to_path on paths (inode, path);
    // no index is needed on stats, because the rows are ordered by the primary key
    pragma user_version=3;
);

bool fakefs_import(const char *archive_path, const char *fs, struct fakefsify_error *err_out, struct progress p) {
    int err = mkdir(fs, 0777);
    if (err < 0)
        POSIX_ERR();

    // make the data root dir
    char path_tmp[PATH_MAX];
    snprintf(path_tmp, sizeof(path_tmp), "%s/data", fs);
    err = mkdir(path_tmp, 0777);
    int root_fd = open(path_tmp, O_RDONLY);
    if (root_fd < 0)
        POSIX_ERR();

    // open the database
    snprintf(path_tmp, sizeof(path_tmp), "%s/meta.db", fs);
    sqlite3 *db;
    err = sqlite3_open_v2(path_tmp, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    CHECK_ERR();
    EXEC("pragma journal_mode=wal")
    EXEC("begin");
    EXEC(schema);

    // open the archive
    struct archive *archive = archive_read_new();
    if (archive == NULL)
        ARCHIVE_ERR(archive);
    archive_read_support_filter_gzip(archive);
    archive_read_support_format_tar(archive);
    if (archive_read_open_filename(archive, archive_path, 65536) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);

    struct stat real_stat;
    if (stat(archive_path, &real_stat) < 0)
        POSIX_ERR();
    size_t archive_bytes = real_stat.st_size;

    sqlite3_stmt *insert_stat = PREPARE("insert into stats (stat) values (?)");
    sqlite3_stmt *insert_path = PREPARE("insert or replace into paths values (?, last_insert_rowid())");

    // do actual shit
    struct archive_entry *entry;
    while ((err = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
        char entry_path[PATH_MAX];
        if (!path_normalize(archive_entry_pathname(entry), entry_path)) {
            // Avoid pwnage
            fprintf(stderr, "warning: skipped possible path traversal %s\n", archive_entry_pathname(entry));
            continue;
        }
        progress_update(&p, (double) archive_filter_bytes(archive, -1) / archive_bytes, entry_path);

        int fd = -1;
        if (archive_entry_filetype(entry) != AE_IFDIR) {
            // mkdir -p
            char *entry_path_copy = strdup(entry_path);
            char *slash = entry_path_copy;
            while ((slash = strchr(slash + 1, '/')) != NULL) {
                *slash = '\0';
                int err = mkdirat(root_fd, fix_path(entry_path_copy), 0777);
                *slash = '/';
                if (err < 0) {
                    if (errno == EEXIST) continue;
                    POSIX_ERR();
                }
                printf("created %s\n", entry_path_copy);
            }
            free(entry_path_copy);

            fd = openat(root_fd, fix_path(entry_path), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0) {
                if (errno == EISDIR) continue; // assuming it's case insensitivity
                POSIX_ERR();
            }
        }
        switch (archive_entry_filetype(entry)) {
            case AE_IFDIR:
                err = mkdirat(root_fd, fix_path(entry_path), 0777);
                if (err < 0 && errno != EEXIST)
                    POSIX_ERR();
                break;
            case AE_IFREG:
                if (archive_read_data_into_fd(archive, fd) != ARCHIVE_OK)
                    ARCHIVE_ERR(archive);
                break;
            case AE_IFLNK:
                err = (int) write(fd, archive_entry_symlink(entry), strlen(archive_entry_symlink(entry)));
                if (err < 0)
                    POSIX_ERR();
        }
        if (fd != -1)
            close(fd);

        struct ish_stat stat = {
            .mode = (mode_t_) archive_entry_mode(entry),
            .uid = (uid_t_) archive_entry_uid(entry),
            .gid = (uid_t_) archive_entry_gid(entry),
            .rdev = (dev_t_) archive_entry_rdev(entry),
        };
        sqlite3_bind_blob64(insert_stat, 1, &stat, sizeof(stat), SQLITE_TRANSIENT);
        STEP_RESET(insert_stat);
        sqlite3_bind_blob64(insert_path, 1, entry_path, strlen(entry_path), SQLITE_TRANSIENT);
        STEP_RESET(insert_path);
    }
    if (err != ARCHIVE_EOF)
        ARCHIVE_ERR(archive);

    FINALIZE(insert_stat);
    FINALIZE(insert_path);
    EXEC("commit");
    sqlite3_close(db);
    close(root_fd);

    if (archive_read_free(archive) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);
    return true;
}

bool fakefs_export(const char *fs, const char *archive_path, struct fakefsify_error *err_out, struct progress p) {
    // open the archive
    struct archive *archive = archive_write_new();
    if (archive == NULL)
        ARCHIVE_ERR(archive);
    archive_write_add_filter_gzip(archive);
    archive_write_set_format_pax_restricted(archive);
    if (archive_write_open_filename(archive, archive_path) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);

    // open the data root dir
    char path_tmp[PATH_MAX];
    snprintf(path_tmp, sizeof(path_tmp), "%s/data", fs);
    int root_fd = open(path_tmp, O_RDONLY);
    if (root_fd < 0)
        POSIX_ERR();

    // open the database
    snprintf(path_tmp, sizeof(path_tmp), "%s/meta.db", fs);
    sqlite3 *db;
    int err = sqlite3_open_v2(path_tmp, &db, SQLITE_OPEN_READONLY, NULL);
    CHECK_ERR();
    EXEC("begin");

    sqlite3_stmt *count_stmt = PREPARE("select count(path) from paths");
    STEP(count_stmt);
    int64_t paths_total = sqlite3_column_int64(count_stmt, 0);
    FINALIZE(count_stmt);
    int64_t paths_done = 0;

    sqlite3_stmt *query = PREPARE("select path, stat from paths, stats using (inode)");
    while (STEP(query)) {
        struct archive_entry *entry = archive_entry_new();

        const char *path_in_db = sqlite3_column_blob(query, 0);
        size_t path_len = sqlite3_column_bytes(query, 0);
        char *path = malloc(path_len + 2);
        path[0] = '.';
        memcpy(path + 1, path_in_db, path_len);
        path[path_len + 1] = '\0';
        archive_entry_set_pathname(entry, path);

        progress_update(&p, (double) paths_done / paths_total, path);

        struct ish_stat stat = *(struct ish_stat *) sqlite3_column_blob(query, 1);
        archive_entry_set_mode(entry, stat.mode);
        archive_entry_set_uid(entry, stat.uid);
        archive_entry_set_gid(entry, stat.gid);
        archive_entry_set_rdev(entry, stat.rdev);

        struct stat real_stat;
        if (fstatat(root_fd, path, &real_stat, 0) < 0) {
            if (errno == ENOENT) {
                printf("skipping %s\n", path);
                goto skip;
            }
            POSIX_ERR();
        }
        archive_entry_set_size(entry, real_stat.st_size);

        int fd = -1;
        if (S_ISREG(stat.mode) || S_ISLNK(stat.mode))
            fd = openat(root_fd, path, O_RDONLY);
        if S_ISLNK(stat.mode) {
            char buf[MAX_PATH+1];
            ssize_t len = read(fd, buf, sizeof(buf)-1);
            if (len < 0)
                POSIX_ERR();
            buf[len] = '\0';
            archive_entry_set_symlink(entry, buf);
        }
        archive_write_header(archive, entry);

        if (S_ISREG(stat.mode)) {
            char buf[8192];
            ssize_t len;
            while ((len = read(fd, buf, sizeof(buf))) > 0) {
                ssize_t written = archive_write_data(archive, buf, len);
                if (written < 0)
                    ARCHIVE_ERR(archive);
                if (written != len)
                    printf("uh oh\n");
            }
            if (len < 0)
                POSIX_ERR();
        }
        if (fd != -1)
            close(fd);

    skip:
        paths_done++;
        free(path);
        archive_entry_free(entry);
    }

    FINALIZE(query);
    sqlite3_close(db);
    close(root_fd);
    if (archive_write_close(archive) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);
    archive_write_free(archive);
    return true;
}
