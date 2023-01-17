#include <iostream>
#include <stdio.h>
#include <fstream>

#include <fileref.h>
#include <tag.h>
#include <tpropertymap.h>

extern "C" {
#include <tfblib/tfblib.h>
#include <tfblib/tfb_colors.h>
}
#include <unistd.h>

#include <thread>
#include <atomic>
// Not in C++17
#include "semaphore.h"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/stat.h>

#include <set>
#include <cmath>

#include <experimental/filesystem>
namespace fs = std::experimental::filesystem; // gcc v7

#include <sqlite3.h>
#include <ssfn.h>

void render_thread();
void touch_thread();
void parse_thread();
void sqlite3_check_err(int code);

// Signals threads to start exiting
std::atomic<bool> exit_thread(false);

// Latest x/y
std::atomic<uint32_t> input_x(0), input_y(0);

// UI state
std::atomic<uint32_t> state(0xff);

// Holds current album/track
std::string should_not_see = "[waiting]";
std::atomic<std::string*> current_loading(&should_not_see);

// Used to block on touch input
// TODO drop inputs while loading/transitioning stuff
Semaphore semaphore{};

// Current directories to render
std::string directories[5] = {};

// ssfn v1 font binary data
std::vector<std::string> font_binary{};

// Not sure what official registration of codes is
// https://www.recordingblogs.com/wiki/format-chunk-of-a-wave-file
const std::map<std::string, int> FORMAT_IDS{
    {".opus", 2373}, {".mp3", 85}, {".flac", 61686},
    {".wav", 1}, {".ogg", 22127}, {".m4a", 278}};

// SQL Queries
const char* SQL_GET_MAX_ID = "SELECT MAX(id) FROM MEDIA_TABLE;";
const char* SQL_UPDATE_COUNT_TABLE1 = "UPDATE COUNT_TABLE SET cn = (SELECT COUNT(*) FROM MEDIA_TABLE) WHERE rowid = 1;";
const char* SQL_UPDATE_COUNT_TABLE2 = "UPDATE COUNT_TABLE SET cn = (SELECT COUNT(*) FROM ALBUM_TABLE) WHERE rowid = 2;";
const char* SQL_UPDATE_COUNT_TABLE3 = "UPDATE COUNT_TABLE SET cn = (SELECT COUNT(*) FROM ARTIST_TABLE) WHERE rowid = 3;";

const char* SQL_INSERT_MEDIA = "INSERT INTO MEDIA_TABLE VALUES(?, ?, ?, ?, ?, '', ?, ?, ?,  0, 0, -1, -1, ?, ?, ?, ?, 16, ?, ?, 0, NULL, NULL, NULL, NULL, ?, ?, ?);";
const char* SQL_INSERT_MEDIA2 = "INSERT INTO MEDIA2_TABLE VALUES(?, ?, ?, ?, ?, '', ?, ?, ?, 0, 0, -1, -1, ?, ?, ?, ?, 16, ?, ?, 0, NULL, NULL, NULL, NULL, ?, ?, ?);";
const char* SQL_INSERT_MTIME = "INSERT INTO MTIME_TABLE VALUES(?);";

const char* SQL_CHECK_ARTIST = "SELECT cn FROM ARTIST_TABLE WHERE artist = ?;";
const char* SQL_INSERT_ARTIST = "INSERT INTO ARTIST_TABLE VALUES(?, ?, ?, ?, ?, ?, ?);";
const char* SQL_INSERT_ARTIST2 = "INSERT INTO ARTIST2_TABLE VALUES(?, ?, ?, ?, ?, ?, ?);";
const char* SQL_UPDATE_ARTIST = "UPDATE ARTIST_TABLE SET cn = ? WHERE artist = ?;";
const char* SQL_UPDATE_ARTIST2 = "UPDATE ARTIST2_TABLE SET cn = ? WHERE artist = ?;";

const char* SQL_CHECK_ALBUM = "SELECT cn FROM ALBUM_TABLE WHERE album = ?;";
const char* SQL_INSERT_ALBUM = "INSERT INTO ALBUM_TABLE VALUES(?, ?, ?, ?, ?, ?, 0, ?);";
// We delete this anyway..
// const char* SQL_INSERT_ALBUM2 = "INSERT INTO ALBUM2_TABLE VALUES(?, ?, ?, ?, ?, ?, 0, ?);";
const char* SQL_UPDATE_ALBUM = "UPDATE ALBUM_TABLE SET cn = ? WHERE album = ?;";
const char* SQL_UPDATE_ALBUM2 = "UPDATE ALBUM2_TABLE SET cn = ? WHERE album = ?;";

// Needs to be sorted (just albums)
const char* SQL_FIX_ALBUM_SORT = "CREATE TABLE ALBUM_TEMP AS SELECT * FROM ALBUM_TABLE ORDER BY album COLLATE NOCASE ASC; DROP TABLE ALBUM_TABLE; ALTER TABLE ALBUM_TEMP RENAME TO ALBUM_TABLE; DROP TABLE ALBUM2_TABLE; CREATE TABLE ALBUM2_TABLE AS SELECT * FROM ALBUM_TABLE;";

int main(int argc, char *argv[]) {
    // Help with logging in case of segfault/kill
    std::cout.setf(std::ios::unitbuf);
    std::cout << "Start\n";

    // Load font(s)
    const std::string fonts[] = {"/mnt/sd_0/font_cjk.sfn"};
    for(auto& font_path : fonts) {
        try {
            std::ifstream font(font_path, std::ios::binary);
            std::ostringstream font_data;
            font_data << font.rdbuf();
            font.close();
            font_binary.push_back(font_data.str());
        } catch (const std::ifstream::failure& e) {
            std::cout << "no font " << font_path << "\n";
            return -1;
        }
    }

    std::cout << "Fonts loaded: " << font_binary.size() << "\n";

    // Kill native GUI and toggle power just in case
    // TODO may still not work if touching/turned off while this is happening...?
    system("kill `ps | grep 'hiby_player.sh' | head -n1 | awk '{print $1}'`");
    system("kill `ps | grep 'system_main_thr' | head -n1 | awk '{print $1}'`");
    usleep(200000);
    system("echo 1 > /sys/class/graphics/fb0/blank");
    usleep(200000);
    system("echo 0 > /sys/class/graphics/fb0/blank");
    usleep(200000);

    // Set up sqlite3 connection
    sqlite3* db;
    if(sqlite3_open_v2("/data/usrlocal_media.db", &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        std::cout << "Could not open db!\n";
        return -1;
    }

    // Start threads
    std::thread render(render_thread);
    std::thread touch(touch_thread);

    // Acquire initial directory list
    typedef struct {
        std::string path;
        time_t tv_sec;
    } direntry;
    const fs::path home{"/mnt/sd_0/"};
    auto comparator = [](const direntry& a, const direntry& b) {
        return a.tv_sec > b.tv_sec; // Order most recent first
    };
    std::set<direntry, decltype(comparator)> entries{comparator};
    struct stat stat_;
    for(auto& entry : fs::directory_iterator{home}) {
        // std::cout << "Discovered " << entry.path().u8string() << "\n";
        if(!fs::is_directory(entry.status())) continue; // Directories only
        stat(entry.path().u8string().c_str(), &stat_);
        entries.insert(direntry{entry.path().u8string().substr(10), stat_.st_mtim.tv_sec});
    }
    std::cout << "Directory has [" << entries.size() << "] entries...";

    // Set up current directory page
    uint32_t page = 0;
    uint32_t local_state = 0;
    auto update_directory_strings = [&]{
        std::cout << "Load page " << page << "\n";
        local_state = 3; // Loading dirs
        state.store(local_state);
        auto it = entries.begin(); auto end = entries.begin();
        if((page + 1) * 5 >= entries.size()) end = entries.end();
        else std::advance(end, (page + 1) * 5);
        std::advance(it, page * 5);
        for(uint32_t i = 0; i < 5; ++i) {
            if(it != entries.end()) {
                directories[i] = (*it).path;
                ++it;
            } else
                directories[i] = " - ";
        }
        usleep(16666*3); // wait for render (also debounce)
        local_state = 0; // Ready
        state.store(local_state);
    };
    update_directory_strings();

    // Load songs from a directory
    std::string current_copy = ""; // Keep alive reference
    sqlite3_stmt* stmt;
    int step_result;
    auto load_songs = [&](uint32_t selected){
        std::string base = "/mnt/sd_0/" + directories[selected];
        std::cout << "Updating " << base << "\n";

        // Get start target media ID
        sqlite3_check_err(sqlite3_prepare_v2(db, SQL_GET_MAX_ID, strlen(SQL_GET_MAX_ID), &stmt, NULL));
        sqlite3_check_err(sqlite3_step(stmt));
        int newId = sqlite3_column_int(stmt, 0);
        sqlite3_check_err(sqlite3_finalize(stmt));
        std::cout << "start ID is " << newId << "\n";

        for(auto& entry : fs::directory_iterator{base}) {
            std::cout << "Reading " << entry.path().u8string() << "\n";
            // std::cout << "ext = " << entry.path().extension().string() << "\n";
            // Check supported filetype
            if(!TagLib::FileRef::defaultFileExtensions()
                    .contains(entry.path().extension().string().substr(1))) continue;

            newId += 1;
            TagLib::FileRef track(entry.path().u8string().c_str());
            current_copy = track.tag()->title().to8Bit(true);
            std::cout << "Title: " << current_copy << "\n";
            current_loading.store(&current_copy);

            // Start DB update (disaster below)
            ////////////////////////////
            stat(entry.path().u8string().c_str(), &stat_);

            ////////// Media

            std::cout << "media" << "\n";
            auto update_media = [&](const char* query) {
            sqlite3_check_err(sqlite3_prepare_v2(db, query, strlen(query), &stmt, NULL));
            sqlite3_check_err(sqlite3_bind_int(stmt, 1, newId)); // ID
            std::string pathWithA = "a:/" + entry.path().u8string().substr(10);
            std::replace(pathWithA.begin(), pathWithA.end(), '/', '\\');
            sqlite3_check_err(sqlite3_bind_text(stmt, 2, pathWithA.data(), pathWithA.size() + 1, SQLITE_TRANSIENT)); // Path
            // TODO don't update if path already in db
            sqlite3_check_err(sqlite3_bind_text(stmt, 3, current_copy.data(), current_copy.size() + 1, SQLITE_TRANSIENT)); // Name
            sqlite3_check_err(sqlite3_bind_text(stmt, 4,
                        track.tag()->album().to8Bit(true).data(),
                        track.tag()->album().to8Bit(true).size() + 1,
                        SQLITE_TRANSIENT)); // Album
            sqlite3_check_err(sqlite3_bind_text(stmt, 5,
                        track.tag()->artist().to8Bit(true).data(),
                        track.tag()->artist().to8Bit(true).size() + 1,
                        SQLITE_TRANSIENT)); // Artist
            sqlite3_check_err(sqlite3_bind_int(stmt, 6, track.tag()->year())); // year
            auto tags = track.tag()->properties().value("DISCNUMBER");
            if(tags.size() > 0)
                sqlite3_check_err(sqlite3_bind_int(stmt, 7, std::stoi(tags[0].toCString()))); // disc
            else
                sqlite3_check_err(sqlite3_bind_int(stmt, 7, 0)); // disc
            sqlite3_check_err(sqlite3_bind_int(stmt, 8, track.tag()->track())); // trackno
            sqlite3_check_err(sqlite3_bind_text(stmt, 9, current_copy.data(), 1, SQLITE_TRANSIENT)); // character TODO properly cut unicode
            sqlite3_check_err(sqlite3_bind_int(stmt, 10, track.file()->length())); // size in bytes
            sqlite3_check_err(sqlite3_bind_int(stmt, 11, track.audioProperties()->sampleRate())); // Hz
            sqlite3_check_err(sqlite3_bind_int(stmt, 12, track.audioProperties()->bitrate())); // bitrate
            sqlite3_check_err(sqlite3_bind_int(stmt, 13, track.audioProperties()->channels())); // channels
            if(FORMAT_IDS.count(entry.path().extension().string()))
                sqlite3_check_err(sqlite3_bind_int(stmt, 14, FORMAT_IDS.at(entry.path().extension().string()))); // ID
            else
                sqlite3_check_err(sqlite3_bind_int(stmt, 14, 0)); // ID
            sqlite3_check_err(sqlite3_bind_int(stmt, 15, stat_.st_ctim.tv_sec)); // create
            sqlite3_check_err(sqlite3_bind_int(stmt, 16, stat_.st_mtim.tv_sec)); // modified
            sqlite3_check_err(sqlite3_bind_text(stmt, 17, current_copy.data(), current_copy.size() + 1, SQLITE_TRANSIENT)); // Name again (pinyin)
            sqlite3_check_err(sqlite3_step(stmt));
            sqlite3_check_err(sqlite3_finalize(stmt));
            };
            update_media(SQL_INSERT_MEDIA);
            update_media(SQL_INSERT_MEDIA2);

            ////////// Album
            std::cout << "album" << "\n";
            sqlite3_check_err(sqlite3_prepare_v2(db, SQL_CHECK_ALBUM, strlen(SQL_CHECK_ALBUM), &stmt, NULL));
            sqlite3_check_err(sqlite3_bind_text(stmt, 1,
                        track.tag()->album().to8Bit(true).data(),
                        track.tag()->album().to8Bit(true).size() + 1,
                        SQLITE_TRANSIENT)); // Album
            step_result = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if(step_result == SQLITE_ROW) { // Exists
                // Add count
                int cn = sqlite3_column_int(stmt, 0) + 1;
                sqlite3_check_err(sqlite3_finalize(stmt));

                sqlite3_check_err(sqlite3_prepare_v2(db, SQL_UPDATE_ALBUM, strlen(SQL_UPDATE_ALBUM), &stmt, NULL));
                sqlite3_check_err(sqlite3_bind_int(stmt, 1, cn));
                sqlite3_check_err(sqlite3_bind_text(stmt, 2,
                            track.tag()->album().to8Bit(true).data(),
                            track.tag()->album().to8Bit(true).size() + 1,
                            SQLITE_TRANSIENT)); // Album
                sqlite3_check_err(sqlite3_step(stmt));
                sqlite3_check_err(sqlite3_finalize(stmt));

                sqlite3_check_err(sqlite3_prepare_v2(db, SQL_UPDATE_ALBUM2, strlen(SQL_UPDATE_ALBUM2), &stmt, NULL));
                sqlite3_check_err(sqlite3_bind_int(stmt, 1, cn));
                sqlite3_check_err(sqlite3_bind_text(stmt, 2,
                            track.tag()->album().to8Bit(true).data(),
                            track.tag()->album().to8Bit(true).size() + 1,
                            SQLITE_TRANSIENT)); // Album
                sqlite3_check_err(sqlite3_step(stmt));
                sqlite3_check_err(sqlite3_finalize(stmt));
            } else {
                // New album
                auto update_new_album = [&](const char* query) {
                sqlite3_check_err(sqlite3_prepare_v2(db, query, strlen(query), &stmt, NULL));
                sqlite3_check_err(sqlite3_bind_int(stmt, 1, newId)); // ID
                sqlite3_check_err(sqlite3_bind_text(stmt, 2,
                            track.tag()->album().to8Bit(true).data(),
                            track.tag()->album().to8Bit(true).size() + 1,
                            SQLITE_TRANSIENT)); // Album
                sqlite3_check_err(sqlite3_bind_text(stmt, 3,
                            track.tag()->album().to8Bit(true).data(),
                            1,
                            SQLITE_TRANSIENT)); // Character
                sqlite3_check_err(sqlite3_bind_int(stmt, 4, 1)); // tracks
                sqlite3_check_err(sqlite3_bind_int(stmt, 5, stat_.st_ctim.tv_sec)); // create
                sqlite3_check_err(sqlite3_bind_int(stmt, 6, stat_.st_mtim.tv_sec)); // modified
                sqlite3_check_err(sqlite3_bind_text(stmt, 7,
                            track.tag()->album().to8Bit(true).data(),
                            track.tag()->album().to8Bit(true).size() + 1,
                            SQLITE_TRANSIENT)); // Pinyin copy
                sqlite3_check_err(sqlite3_step(stmt));
                sqlite3_check_err(sqlite3_finalize(stmt));
                };
                update_new_album(SQL_INSERT_ALBUM);
                // update_new_album(SQL_INSERT_ALBUM2);

                // Instead we need to just fix(resort) current tables here
                char* errmsg = NULL;
                sqlite3_exec(db, SQL_FIX_ALBUM_SORT, NULL, NULL, &errmsg);
                if(errmsg != NULL) {
                    std::cout << "Failed to fix album sort - " << errmsg << "\n";
                    free(errmsg);
                }
            }

            ////////// Artist
            std::cout << "artist" << "\n";
            sqlite3_check_err(sqlite3_prepare_v2(db, SQL_CHECK_ARTIST, strlen(SQL_CHECK_ARTIST), &stmt, NULL));
            sqlite3_check_err(sqlite3_bind_text(stmt, 1,
                        track.tag()->artist().to8Bit(true).data(),
                        track.tag()->artist().to8Bit(true).size() + 1,
                        SQLITE_TRANSIENT)); // Artist
            step_result = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if(step_result == SQLITE_ROW) { // Exists
                // Add count
                int cn = sqlite3_column_int(stmt, 0) + 1;
                sqlite3_check_err(sqlite3_finalize(stmt));

                sqlite3_check_err(sqlite3_prepare_v2(db, SQL_UPDATE_ARTIST, strlen(SQL_UPDATE_ARTIST), &stmt, NULL));
                sqlite3_check_err(sqlite3_bind_int(stmt, 1, cn));
                sqlite3_check_err(sqlite3_bind_text(stmt, 2,
                            track.tag()->artist().to8Bit(true).data(),
                            track.tag()->artist().to8Bit(true).size() + 1,
                            SQLITE_TRANSIENT)); // Artist
                sqlite3_check_err(sqlite3_step(stmt));
                sqlite3_check_err(sqlite3_finalize(stmt));

                sqlite3_check_err(sqlite3_prepare_v2(db, SQL_UPDATE_ARTIST2, strlen(SQL_UPDATE_ARTIST), &stmt, NULL));
                sqlite3_check_err(sqlite3_bind_int(stmt, 1, cn));
                sqlite3_check_err(sqlite3_bind_text(stmt, 2,
                            track.tag()->artist().to8Bit(true).data(),
                            track.tag()->artist().to8Bit(true).size() + 1,
                            SQLITE_TRANSIENT)); // Artist
                sqlite3_check_err(sqlite3_step(stmt));
                sqlite3_check_err(sqlite3_finalize(stmt));
            } else {
                // New artist
                auto update_new_artist = [&](const char* query) {
                sqlite3_check_err(sqlite3_prepare_v2(db, query, strlen(query), &stmt, NULL));
                sqlite3_check_err(sqlite3_bind_int(stmt, 1, newId)); // ID
                sqlite3_check_err(sqlite3_bind_text(stmt, 2,
                            track.tag()->artist().to8Bit(true).data(),
                            track.tag()->artist().to8Bit(true).size() + 1,
                            SQLITE_TRANSIENT)); // Artist
                sqlite3_check_err(sqlite3_bind_text(stmt, 3,
                            track.tag()->artist().to8Bit(true).data(),
                            1,
                            SQLITE_TRANSIENT)); // Character
                sqlite3_check_err(sqlite3_bind_int(stmt, 4, 1)); // tracks
                sqlite3_check_err(sqlite3_bind_int(stmt, 5, stat_.st_ctim.tv_sec)); // create
                sqlite3_check_err(sqlite3_bind_int(stmt, 6, stat_.st_mtim.tv_sec)); // modified
                sqlite3_check_err(sqlite3_bind_text(stmt, 7,
                            track.tag()->artist().to8Bit(true).data(),
                            track.tag()->artist().to8Bit(true).size() + 1,
                            SQLITE_TRANSIENT)); // Pinyin copy
                sqlite3_check_err(sqlite3_step(stmt));
                sqlite3_check_err(sqlite3_finalize(stmt));
                };
                update_new_artist(SQL_INSERT_ARTIST);
                update_new_artist(SQL_INSERT_ARTIST2);
            }

            ////////// Mtime
            std::cout << "mtime" << "\n";
            sqlite3_check_err(sqlite3_prepare_v2(db, SQL_INSERT_MTIME, strlen(SQL_INSERT_MTIME), &stmt, NULL));
            sqlite3_check_err(sqlite3_bind_int(stmt, 1, newId));
            sqlite3_check_err(sqlite3_step(stmt));
            sqlite3_check_err(sqlite3_finalize(stmt));
            ////////////////////////////
        }
        // Update counts
        std::cout << "counts" << "\n";
        sqlite3_check_err(sqlite3_prepare_v2(db, SQL_UPDATE_COUNT_TABLE1, strlen(SQL_UPDATE_COUNT_TABLE1), &stmt, NULL));
        sqlite3_check_err(sqlite3_step(stmt));
        sqlite3_check_err(sqlite3_finalize(stmt));
        sqlite3_check_err(sqlite3_prepare_v2(db, SQL_UPDATE_COUNT_TABLE2, strlen(SQL_UPDATE_COUNT_TABLE2), &stmt, NULL));
        sqlite3_check_err(sqlite3_step(stmt));
        sqlite3_check_err(sqlite3_finalize(stmt));
        sqlite3_check_err(sqlite3_prepare_v2(db, SQL_UPDATE_COUNT_TABLE3, strlen(SQL_UPDATE_COUNT_TABLE3), &stmt, NULL));
        sqlite3_check_err(sqlite3_step(stmt));
        sqlite3_check_err(sqlite3_finalize(stmt));

        current_loading.store(&should_not_see);
    };
    std::cout << "Extensions: " << TagLib::FileRef::defaultFileExtensions().toString(", ") << "\n";

    // Local touch x, y copy and current selected directory index
    uint32_t x, y, selected(0);

    // Start UI
    state.store(local_state);

    /*
     * Monitor input and perform actions as set
     */
    while(1) {
        // Wait for some valid touch input
        // std::cout << "Main thread wait\n";
        semaphore.acquire();
        // std::cout << "Main thread read\n";
        // TODO Can be raced that another touch happens inbetween acquire and here s.t. another trigger is queued again but the input x/y is duplicated
        x = input_x.load();
        y = input_y.load();
        // std::cout << "Main thread " << x << ", " << y << " @ " << local_state << "\n";

        // If statement allows for break...
        if(local_state == 0) { // Index
            // Up/down
            if(y >= 430 && x > 150 && x < 290 && (page + 1) * 5 < entries.size()) {
                std::cout << "Page up\n";
                page += 1;
                update_directory_strings();
            }
            if(y >= 430 && x < 140 && page > 0) {
                std::cout << "Page down\n";
                page -= 1;
                update_directory_strings();
            }

            if(y >= 125 && y < 375) { // Select a directory
                selected = (y - 125) / 50;
                if(directories[selected] == " - ") continue;
                current_loading.store(&directories[selected]);
                local_state = 1;
                state.store(local_state);
            }

            if(y > 430 && x > 310) { // Exit
                exit_thread.store(true);
                break;
            }
        } else if(local_state == 1) { // Confirm yes/no
            if(y >= 430 && x < 140) { // Yes
                local_state = 2;
                state.store(local_state);
                // Load songs
                load_songs(selected);
                
                // Done
                local_state = 0;
                state.store(local_state);
            }

            if(y >= 430 && x > 150 && x < 290) { // Yes
                local_state = 0;
                state.store(local_state);
            }
        }
    }

    // Signal exit
    exit_thread.store(true);
    sleep(1);
    if(touch.joinable())
        touch.join();
    if(render.joinable())
        render.join();

    // Close db
    sqlite3_close(db);

    std::cout << "End\n";

    // Relaunch native UI
    system("( nohup /bin/sh /usr/bin/hiby_player.sh & )");
    exit(0);
}

/////////////////////////

// Cuts off at w, h
void draw_string(ssfn_t& ctx, uint32_t x, uint32_t y, std::string str, uint32_t fg, uint32_t bg, uint32_t w, uint32_t h) {
    // std::cout << "rendering " << str << "\n";
    ssfn_glyph_t *glyph;
    char* ptr = str.data();

    // Processed bytes in string
    ssize_t processed = 0;

    // r/g/b uint_8 of fg and bg as double for scaling
    double fg_r = (double)((fg >> 16) & 0xff);
    double fg_g = (double)((fg >> 8) & 0xff);
    double fg_b = (double)((fg >> 0) & 0xff);
    double bg_r = (double)((bg >> 16) & 0xff);
    double bg_g = (double)((bg >> 8) & 0xff);
    double bg_b = (double)((bg >> 0) & 0xff);

    // While there are characters left...
    while(ptr < (str.c_str() + str.size()) && x < w && y < h) {
        // Get code point
        uint32_t code = ssfn_utf8(&ptr);

        // Render
        glyph = ssfn_render(&ctx, code);
        if(glyph == NULL) {
            std::cout << "failed to render glyph " << code << " - " << ssfn_error(ssfn_lasterr(&ctx)) << "\n";
            continue;
        }

        // Draw glyph
        for(uint32_t Y = 0; Y < glyph->h && (Y + y - glyph->baseline) < h && (Y + y - glyph->baseline) >= 0; ++Y) {
            for(uint32_t X = 0; X < glyph->w && (X + x) < w; ++X) {
                uint8_t amt = (*(glyph->data + glyph->pitch * Y + X));
                if(amt == 0) continue; // skip assumed pre-drawn background box

                // out = fg * amt/255 + bg * (1 - amt/255)
                double frac = (double)amt / 255.;
                uint8_t color_r = std::round(frac * fg_r + (1. - frac) * bg_r);
                uint8_t color_g = std::round(frac * fg_g + (1. - frac) * bg_g);
                uint8_t color_b = std::round(frac * fg_b + (1. - frac) * bg_b);

                tfb_draw_pixel(X + x, Y + y - glyph->baseline, tfb_make_color(color_r, color_g, color_b));
            }
        }

        x += glyph->adv_x;
        free(glyph);
    }
}

void render_thread() {
    std::cout << "hi from render thread\n";
    int rc;

    if ((rc = tfb_acquire_fb(TFB_FL_USE_DOUBLE_BUFFER, NULL, NULL)) != TFB_SUCCESS) {
        std::cout << "tfb_acquire_fb() failed: " << tfb_strerror(rc) << "\n";
      return;
    }

    uint32_t w = tfb_screen_width();
    uint32_t h = tfb_screen_height();
    uint32_t rect_w = w / 2;
    uint32_t rect_h = h / 2;

    tfb_clear_screen(tfb_black);
    tfb_draw_string(10, 10, tfb_white, tfb_black, "Initializing...");
    tfb_flush_window();
    tfb_flush_fb();
    usleep(200000);

    // Load ssfn fonts in
    ssfn_t ctx = {0};
    for(auto& font_binary_i : font_binary) {
        ssfn_load(&ctx, (ssfn_font_t*)font_binary_i.c_str());
    }
    auto set_font_size = [&](uint32_t size) {
        ssfn_select(&ctx,
                SSFN_FAMILY_ANY, NULL,
                SSFN_STYLE_REGULAR, size, SSFN_MODE_ALPHA);
    };

    uint32_t state_cache(1); // set first
    const char* cache_loading = NULL;

    while(!exit_thread.load() && usleep(16666) == 0) {
        if(state.load() != state_cache) {
            state_cache = state.load();
            tfb_clear_screen(tfb_black);
            switch(state_cache) {
                case 0:
                // Index
                set_font_size(36);
                draw_string(ctx, 15, 50, u8"Hello!", tfb_white, tfb_black, w, h);

                // Directories
                set_font_size(20);
                tfb_draw_rect(5, 125, w - 5, 50, tfb_magenta);
                draw_string(ctx, 10, 156, directories[0].data(), tfb_red, tfb_black, w, h);
                tfb_draw_rect(5, 175, w - 5, 50, tfb_magenta);
                draw_string(ctx, 10, 206, directories[1].data(), tfb_red, tfb_black, w, h);
                tfb_draw_rect(5, 225, w - 5, 50, tfb_magenta);
                draw_string(ctx, 10, 256, directories[2].data(), tfb_red, tfb_black, w, h);
                tfb_draw_rect(5, 275, w - 5, 50, tfb_magenta);
                draw_string(ctx, 10, 306, directories[3].data(), tfb_red, tfb_black, w, h);
                tfb_draw_rect(5, 325, w - 5, 50, tfb_magenta);
                draw_string(ctx, 10, 356, directories[4].data(), tfb_red, tfb_black, w, h);
                
                
                set_font_size(20);
                // Up/Down buttons
                tfb_fill_rect(0, 430, 140, 50, tfb_indigo);
                draw_string(ctx, 50, 460, u8"Down", tfb_white, tfb_indigo, w, h);
                tfb_fill_rect(150, 430, 140, 50, tfb_indigo);
                draw_string(ctx, 210, 460, u8"Up", tfb_white, tfb_indigo, w, h);

                // Exit button
                tfb_fill_rect(300, 430, 60, 50, tfb_indigo);
                draw_string(ctx, 312, 460, u8"Exit", tfb_white, tfb_indigo, w, h);

                break;
                case 1:
                // Confirm
                set_font_size(36);
                draw_string(ctx, 15, 50, u8"Update this", tfb_white, tfb_black, w, h);
                draw_string(ctx, 15, 122, u8"directory?", tfb_white, tfb_black, w, h);

                set_font_size(16);
                draw_string(ctx, 15, 250, current_loading.load()->data(), tfb_white, tfb_black, w, h);
                
                set_font_size(20);
                tfb_fill_rect(0, 430, 140, 50, tfb_indigo);
                draw_string(ctx, 50, 460, u8"Yes", tfb_white, tfb_indigo, w, h);
                tfb_fill_rect(150, 430, 140, 50, tfb_indigo);
                draw_string(ctx, 210, 460, u8"No", tfb_white, tfb_indigo, w, h);
                break;
                case 2:
                // Loading songs (alternate to trigger screen update)
                set_font_size(36);
                draw_string(ctx, 15, 50, u8"Loading...", tfb_white, tfb_black, w, h);

                cache_loading = NULL;
                while(*current_loading.load() != should_not_see) {
                    while(cache_loading == current_loading.load()->data()
                            && usleep(1000) == 0) {}
                    cache_loading = current_loading.load()->data();
                    tfb_fill_rect(15, 100, w - 15, 300, tfb_black);
                    set_font_size(20);
                    draw_string(ctx, 15, 250, cache_loading, tfb_white, tfb_black, w, h);
                    tfb_flush_window();
                    tfb_flush_fb();
                }
                break;
                case 3:
                // Loading dirs
                tfb_draw_string(10, 10, tfb_white, tfb_black, "Loading directories...");
                break;
                case 0xff:
                // Still initializing
                break;
            }
            tfb_flush_window();
            tfb_flush_fb();
        }
        tfb_flush_window();
    }
    
    // Exiting
    tfb_clear_screen(tfb_blue);
    tfb_draw_string(10, 10, tfb_white, tfb_black, "Done. Please wait for GUI restart...");
    tfb_draw_string(10, 40, tfb_white, tfb_black, "You may want to restart the device.");
    tfb_flush_window();
    tfb_flush_fb();

    tfb_release_fb();
    std::cout << "bye from render thread\n";
}

void touch_thread() {
    std::cout << "hi from touch thread\n";
    auto fd = open("/dev/input/event2", O_RDONLY);
    if(errno != 0) {
        std::cout << "Can't open touchscreen " << strerror(errno) << "\n";
        return;
    }

    struct input_event ev;
    const size_t ev_size = sizeof(struct input_event);
    ssize_t size;

    struct pollfd monitor;
    monitor.fd = fd;
    monitor.events = POLLIN;
    int ready;

    while(!exit_thread.load()) {
        ready = poll(&monitor, 1, 500);
        // std::cout << "poll returned " << ready << "\n";
        if(ready == 0) continue;
        if(ready < 0) {
            std::cout << "poll error " << ready << "\n";
            return;
        }

        size = read(fd, &ev, ev_size);
        if (size < ev_size) {
            std::cout << "touchscreen event error (wrong size)\n";
            return;
        }

        // X always reported first
        if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X)
            input_x.store(ev.value);
        if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) {
            input_y.store(ev.value);
            // std::cout << "Touched " << input_x.load() << ", " << input_y.load() << "\n";
            semaphore.release();
        }
    }

    close(fd);
    std::cout << "bye from touch thread\n";
}

void sqlite3_check_err(int code) {
    if(code != SQLITE_OK && code != SQLITE_ROW && code != SQLITE_DONE) {
        std::cout << "sqlite error code: " << sqlite3_errstr(code) << "\n";
    }
}
