#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "fatfs_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "sdcard_list.h"
#include "sdcard_scan.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "board.h"

static const char *TAG = "BOOTLEG_SPOTIFY_ON_SD_CARD";

#define SDCARD_MAX_SIZE 2048
#define MAX_PLAYLIST_COLLECTION_SIZE = 99;

typedef struct sdcard_list {
    char *save_file_name;                // Name of file to save URLs
    char *offset_file_name;              // Name of file to save offset
    FILE *save_file;                     // File to save urls
    FILE *offset_file;                   // File to save offset of urls
    char *cur_url;                       // Point to current URL
    uint16_t url_num;                    // Number of URLs
    uint16_t cur_url_id;                 // Current url ID
    uint32_t total_size_save_file;       // Size of file to save URLs
    uint32_t total_size_offset_file;     // Size of file to save offset
} sdcard_list_t;

char** playList = NULL;
char*** playListCollection = NULL;
int amount_of_playlist = 0;

audio_pipeline_handle_t pipeline;
audio_event_iface_handle_t evt;
esp_periph_set_handle_t set;
audio_element_handle_t http_stream_reader, i2s_stream_writer, mp3_decoder, fatfs_stream_reader, rsp_handle, i2s_stream_reader;
playlist_operator_handle_t sdcard_list_handle = NULL;


void init_sd_card();

void sdcard_url_save_cb();
void add_song();
void play_song();
void create_playlist();
void shuffle_random_playlist();
void shuffle_last_created_playlist();
void shuffle_playlist();
char** get_playList();
char** get_last_created_playlist();
void audio_stop();


//Initialising of the SDcard
void sdcard_url_save_cb(void *user_data, char *url){

    playlist_operator_handle_t sdcard_handle = (playlist_operator_handle_t)user_data;
    esp_err_t return_value = sdcard_list_save(sdcard_handle, url);

    //We got an error
    if (return_value != ESP_OK){
        ESP_LOGE(TAG, "Fail to save sdcard url to sdcard playlist");
    }
}

//Add a song with a given URL
void add_song(char* url) {
    //Check if song does not exist
    if(sdcard_list_exist(sdcard_list_handle, url) == 0) {
        //Save url to SD
        sdcard_list_save(sdcard_list_handle, url);
    } else {
        ESP_LOGE(TAG, "URL %s is already on SD card", url);
    }
}

//Play song with a given URL and Dir
void play_song(char* dir, char* url){

    //Extend the url, so the SDcard can find the number needs to be format: file://sdcard/'dir'/'url'.mp3
    char full_url[SDCARD_MAX_SIZE];
    strcpy(full_url, "file://sdcard/");
    strcat(full_url, dir);
    strcat(full_url, "/");
    strcat(full_url, url);
    strcat(full_url, ".mp3");
    
    ESP_LOGI(TAG, "Got song: %s", full_url);

    //Stops the current song, before playing new one
    if(pipeline != NULL) {
        audio_pipeline_stop(pipeline);
        audio_pipeline_wait_for_stop(pipeline);
        audio_pipeline_terminate(pipeline);

        audio_element_set_uri(fatfs_stream_reader, full_url);
        audio_pipeline_reset_ringbuffer(pipeline);
        audio_pipeline_reset_elements(pipeline);
        audio_pipeline_change_state(pipeline, AEL_STATE_INIT);
        audio_pipeline_run(pipeline);

        ESP_LOGI(TAG, "Song %s is playing", url);
    }
}

//Creates a new playlist
void create_playlist(char* dir) {
    
    //Gives the prefix "/sdcard/", so the SDcard can find the songs
    char sdcard_dir[SDCARD_MAX_SIZE];
    strcpy(sdcard_dir, "/sdcard/");
    strcat(sdcard_dir, dir);

    sdcard_scan(sdcard_url_save_cb, sdcard_dir, 1, (const char *[]) {"mp3"}, 1, sdcard_list_handle);

    sdcard_list_t *handle_playlist = sdcard_list_handle->playlist;

    //If playList contains songs
    if(playList->url_num != 0) {
        int32_t pos = 0;
        int16_t size = 0;
        char* url = calloc(1, SDCARD_MAX_SIZE);

        //plus two, because first two slots are occupied with size and dir
        playList = calloc(handle_playlist->url_num + 2, SDCARD_MAX_SIZE);

        fseek(handle_playlist->save_file, 0, SEEK_SET);
        fseek(handle_playlist->offset_file, 0, SEEK_SET);

        //Save songList size on 0 position and dir on 1
        playList[0] = (char*) handle_playlist->url_num;
        playList[1] = dir;

   
        //Loop trough all the songs
        for(int i = 0; i < handle_playlist->url_num; i++){
            //Set memory for the given url
            memset(url, 0, SDCARD_MAX_SIZE)

            //Seek and read files on the given directory
            fread(&pos, 1, sizeof(uint32_t), handle_playlist->offset_file);
            fread(&size, 1, sizeof(uint16_t), handle_playlist->offset_file);
            fseek(handle_playlist->save_file, pos, SEEK_SET) ;
            fread(url, 1, size, handle_playlist->save_file);

            //Copy's the url so the array doesnt point to a pointer.
            char *temp_url = calloc(1, SDCARD_MAX_SIZE);
            //Gets the 14'th char, because we dont want the file dir to go with it
            strcpy(temp_url, url + 14);

            //Removes the suffix .mp3
            int length = strlen(temp_url);
            //Adds \0 to mark end of the string
            temp_url[length-4] = '\0';

            //Adds url to array
            playList[i + 2] = temp_url;

            free(temp_url);
        
        }

        //We want to save the playList in the playListCollection
        //Need to allocate new memory first
        amount_of_playlist += 1;
        playListCollection = realloc(playListCollection, amount_of_playlist * handle_playlist->url_num * SDCARD_MAX_SIZE);

        //Put playList in playListCollection
        playListCollection[amount_of_playlist -1] = playList;

        free(url);

    } else {

        ESP_LOGE(TAG, "DIR %s does not contain any songs!", dir);
    }
}

//Shuffles a randomly chosen playlist
void shuffle_random_playList() {

    if(amount_of_playlist > 0) {
        //Get random number
        int random_int = rand() % amount_of_playlist;

        //Shuffle random playlist
        shuffe_playlist(playListCollection[random_int]);
        
    } else {
        ESP_LOGE(TAG, "No playList created for shuffeling yet!")
    }

}

//Shuffles the last created playlist
void shuffe_last_created_playlist() {

    //Shuffles the last created playlist
    if(playList != NULL) {
        shuffe_playlist(playList);
    }
    else {
        ESP_LOGE(TAG, "Playlist not created yet!");
    }
}


//Shuffles a given playList
void shuffle_playlist(char** givenPlayList) {

    //Atoi converts char* to int (stdlib.h)
    int size = atoi(givenPlayList[0]);
    char* dir = givenPlayList[1];

    int random_int;
    int unique_song = 0;
    int unplayed_song_size = size - 2;
    
    //fill array with values of size + 1. This way, we will have a value that will be out of array bounds.
    //is needed for psuedo random check
    int keep_count_of_played_songs[size] = {[0 ... size] = size + 1};

    //Play songs until all songs are played
    while (unplayed_song_size != 0)
    {
        //Create a pseudo random int for shuffeling
        while(unique_song == 0) {

            //Create random int in the scope of array
            random_int = rand() % size;
            unique_song = 1;

            //Look if int already exists
            for(int i = 0; i < size; i++) {

                //If we find a match, it means the played song is not unique
                //Exclude 0 and 1 because of playList structure
                if(random_int == keep_count_of_played_songs[i] && random_int != 0 && random_int != 1) { 
                    unique_song = 0;
                    break;
                }
            }

            //If we found a unique song, we need to save the song value
            if(unique_song = 1) {
                //Loops trough array again
                for(int i = 0; i < size; i++) {
                    //If value is pre-set
                    if(keep_count_of_played_songs[i] == size+1) {
                        //Overwrite it with the song number
                        keep_count_of_played_songs[i] = random_int;
                    }
                }
            }
        }
        play_song(dir, givenPlayList[random_int]);
        unplayed_song_size--;
    }
}


//Returns a playList with given index
char** get_playlist(int index) {

    //Checks if index is in bounds
    if(index <= amount_of_playlist && index > -1) {
        return playListCollection[index];
    } else {

        ESP_LOGE(TAG, "Index %d is out of bounds!", index);
        char** tempPlayList = calloc(3, SDCARD_MAX_SIZE);

        //Return a Error Playlist
        tempPlayList[0] = "3";
        tempPlayList[1] = "NULL";
        tempPlayList[2] = "Error when calling playlist!";
        return tempPlayList;
    }
}

//Returns last created playlist
char** get_last_created_playlist(){
    if(playList == NULL){
        ESP_LOGE(TAG, "Playlist not created yet!");

        //PlayList doesn't exist/have any songs so we return an array with a warning.
        char** tempPlayList = calloc(3, SDCARD_MAX_SIZE);
        //Return a Error Playlist
        tempPlayList[0] = "3";
        tempPlayList[1] = "NULL";
        tempPlayList[2] = "No playList created yet!";
        return playList;
    } else {
        return playList;
    }
}


//IMPORTANT, NEEDS TO BE CALLED FIRST!! CONSTRUCTOR
void init_sd_card(void)
{
   //Init of the SD card, Code not written by self, but example code (r306 - r374)

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "[ 1 ] Mount sdcard");
    // Initialize peripherals management
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    set = esp_periph_set_init(&periph_cfg);

    // Initialize SD Card peripheral
    audio_board_key_init(set);
    audio_board_sdcard_init(set, SD_MODE_1_LINE);

    ESP_LOGI(TAG, "[1.2] Set up a sdcard playlist and scan sdcard music save to it");
    sdcard_list_create(&sdcard_list_handle);
    sdcard_scan(sdcard_url_save_cb, "/sdcard", 0, (const char *[]) {"mp3"}, 1, sdcard_list_handle);

    ESP_LOGI(TAG, "[ 2 ] Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[3.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "[3.1] Create fatfs stream to read data from sdcard");
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_READER;
    fatfs_stream_reader = fatfs_stream_init(&fatfs_cfg);

    ESP_LOGI(TAG, "[3.2] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[3.3] Create mp3 decoder to decode mp3 file");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_decoder = mp3_decoder_init(&mp3_cfg);

    ESP_LOGI(TAG, "[3.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, fatfs_stream_reader, "file");
    audio_pipeline_register(pipeline, mp3_decoder, "mp3");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[3.5] Link it together [sdcard]-->fatfs_stream-->mp3_decoder-->i2s_stream-->[codec_chip]");
    const char *link_tag[3] = {"file", "mp3", "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 3);

    ESP_LOGI(TAG, "[3.6] Set up  uri (file as fatfs_stream, mp3 as mp3 decoder, and default output is i2s)");
    audio_element_set_uri(fatfs_stream_reader, "/sdcard/test.mp3");

    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);

    ESP_LOGI(TAG, "[ 6 ] Listen for all pipeline events");

    //End of init, code written again

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }
    }

    audio_stop();
}


//Not self written.
void audio_stop(){


    //Stops the audio, also not written, but copied from example (r393 - r419)

    ESP_LOGI(TAG, "[ 7 ] Stop audio_pipeline");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);

    audio_pipeline_unregister(pipeline, fatfs_stream_reader);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_unregister(pipeline, mp3_decoder);

    /* Terminal the pipeline before removing the listener */
    audio_pipeline_remove_listener(pipeline);

    /* Stop all periph before removing the listener */
    esp_periph_set_stop_all(set);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(fatfs_stream_reader);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(mp3_decoder);
    esp_periph_set_destroy(set);

    free(playListCollection);
    free(playList);
}