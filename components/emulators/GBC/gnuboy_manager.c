/*********************
 *      LIBRARIES
 *********************/
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "user_input.h"
#include "display_HAL.h"
#include "system_configuration.h"
#include "system_manager.h"
#include "sound_driver.h"

// GNUBoy libraries

#include <loader.h>
#include <hw.h>
#include <lcd.h>
#include <fb.h>
#include <cpu.h>
#include <pcm.h>
#include <regs.h>
#include <rtc.h>
#include <gnuboy.h>
#include <sound.h>

/*********************
 *      DEFINES
 *********************/


/**********************
 *  STATIC PROTOTYPES
 **********************/
static void audioTask(void *arg);
static void run_to_vblank();
static void videoTask(void *arg);
static void gnuBoyTask(void *arg);
static void input_set();

/**********************
 *  TASK HANDLERS
 **********************/
TaskHandle_t videoTask_handler;
TaskHandle_t audioTask_handler;
TaskHandle_t gnuBoyTask_handler;

/**********************
 *  QUEUE HANDLERS
 **********************/
QueueHandle_t vidQueue;
QueueHandle_t audioQueue;

/**********************
 *   GLOBAL VARIABLES
 **********************/

bool game_running = false;
static const char *TAG = "gnuBoy_manager";



struct fb fb;
struct pcm pcm;

uint16_t *displayBuffer[2];
uint8_t currentBuffer;

uint16_t *framebuffer;
int frame = 0;
uint elapsedTime = 0;

volatile bool videoTaskIsRunning = false;


////////////////////////////////////////////////
unsigned char *audioBuffer[2];
volatile uint8_t currentAudioBuffer = 0;
volatile uint16_t currentAudioSampleCount;
volatile unsigned char *currentAudioBufferPtr;

char * game_name;

#define AUDIO_SAMPLE_RATE (16000)

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void gnuboy_start(const char *name, uint8_t console){
    
    ESP_LOGI(TAG,"Executing GameBoy Color game: %s",name);

    //Save game name to use by save and load functions
    game_name = malloc(strlen(name));
    strcpy(game_name,name);

    // Queue creation
    vidQueue = xQueueCreate(7, sizeof(uint16_t *));
    audioQueue = xQueueCreate(1, sizeof(uint16_t *));

    // Load game from the SD card and save on RAM.
    gbc_rom_load(game_name,console);
   // gbc_state_load(0);
   
    //Execute emulator tasks.
    xTaskCreatePinnedToCore(&videoTask, "videoTask", 1024*2, NULL, 1, &videoTask_handler, 1);
    xTaskCreatePinnedToCore(&audioTask, "audioTask", 2048, NULL, 1, &audioTask_handler, 1);
    xTaskCreatePinnedToCore(&gnuBoyTask, "gnuboyTask", 3048, NULL, 1, &gnuBoyTask_handler, 0);

}

void gnuboy_resume(){

    ESP_LOGI(TAG,"GameBoy Color Resume");
    vTaskResume(videoTask_handler);
    vTaskResume(audioTask_handler);
    vTaskResume(gnuBoyTask_handler);

}

void gnuboy_suspend(){

    ESP_LOGI(TAG,"GameBoy Color Suspend");
    vTaskSuspend(gnuBoyTask_handler);
    vTaskSuspend(videoTask_handler);
    vTaskSuspend(audioTask_handler);
    
}

void gnuboy_save(){
    // Create save file name
    gbc_state_save(0);

}

void gnuboy_load(){
    gbc_state_load(0);
   // gnuboy_start("mario.gbc");
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void videoTask(void *arg){

    ESP_LOGI(TAG, "GNUBoy Video Task Initialize");
    uint16_t *param;
    display_HAL_gb_frame(NULL);
    
    while(1){
        xQueuePeek(vidQueue, &param, portMAX_DELAY);
        display_HAL_gb_frame(param);
        xQueueReceive(vidQueue, &param, portMAX_DELAY);

        vTaskDelay(2 / portTICK_RATE_MS);
    }

    ESP_LOGE(TAG,"GNUBoy video task fatal error");
    vTaskDelete(NULL);
}

static void audioTask(void *arg){

    ESP_LOGI(TAG, "GNUBoy Audio Task Initialize");
    uint16_t *param;

    while(1){
        xQueuePeek(audioQueue, &param, portMAX_DELAY);
        audio_submit(currentAudioBufferPtr, currentAudioSampleCount >> 1);
        xQueueReceive(audioQueue, &param, portMAX_DELAY);
    }

}

static void gnuBoyTask(void *arg){

    ESP_LOGI(TAG, "Initialize GNUBoy task");

    // Allocate the memory for the display buffer
    
   // printf("Memory Internal %i\r\n",system_memory(MEMORY_INTERNAL));
   // printf("Memory DMA %i\r\n",system_memory(MEMORY_DMA));
    displayBuffer[0] = heap_caps_malloc(160 * 144 * 2,MALLOC_CAP_8BIT | MALLOC_CAP_DMA );
    if(displayBuffer[0] == NULL){
        printf("Buffer 0 malloc erro\r\n");
        displayBuffer[0] = malloc(160 * 144 * 2);
    }
  //  printf("Memory Internal %i\r\n",system_memory(MEMORY_INTERNAL));
  //  printf("Memory DMA %i\r\n",system_memory(MEMORY_DMA));
    displayBuffer[1] = heap_caps_malloc(160 * 144 * 2,MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL );
    if(displayBuffer[1] == NULL){
        printf("Buffer 1 malloc erro\r\n");
        displayBuffer[1] = malloc(160 * 144 * 2);
    }
   // printf("Memory Internal %i\r\n",system_memory(MEMORY_INTERNAL));
   // printf("Memory DMA %i\r\n",system_memory(MEMORY_DMA));

    if(displayBuffer[0] == 0 || displayBuffer[1] == 0){
        printf("Free init %i bytes\r\n",esp_get_free_heap_size());
        ESP_LOGE(TAG, "Display Buffer allocation fail: displayBuffer[0]=%p, [1]=%p",displayBuffer[0], displayBuffer[1]);
        abort();
    }

    //Clean the buffers
    memset(displayBuffer[0], 0, 160 * 144 * 2);
    memset(displayBuffer[1], 0, 160 * 144 * 2);
    

    framebuffer = displayBuffer[0];

    emu_reset();

    rtc.d = 1;
    rtc.h = 1;
    rtc.m = 1;
    rtc.s = 1;
    rtc.t = 1;

    // vid_begin
    memset(&fb, 0, sizeof(fb));
    fb.w = 160;
    fb.h = 144;
    fb.pelsize = 2;
    fb.pitch = fb.w * fb.pelsize;
    fb.indexed = 0;
    fb.ptr = framebuffer;
    fb.enabled = 1;
    fb.dirty = 0;

         // Note: Magic number obtained by adjusting until audio buffer overflows stop.
    const int audioBufferLength = AUDIO_SAMPLE_RATE / 10 + 1;
    printf("CHECKPOINT AUDIO: HEAP:0x%x - allocating 0x%x\n", esp_get_free_heap_size(), audioBufferLength * sizeof(int16_t) * 2 * 2);
    const int AUDIO_BUFFER_SIZE = audioBufferLength * sizeof(int16_t) * 2;

    // pcm.len = count of 16bit samples (x2 for stereo)
    memset(&pcm, 0, sizeof(pcm));
    pcm.hz = AUDIO_SAMPLE_RATE;
    pcm.stereo = 1;
    pcm.len =  audioBufferLength;
    pcm.buf = malloc(AUDIO_BUFFER_SIZE);
    pcm.pos = 0;

    audioBuffer[0] = pcm.buf;
    audioBuffer[1] = malloc(AUDIO_BUFFER_SIZE);

    if (audioBuffer[0] == 0 || audioBuffer[1] == 0){
        ESP_LOGE(TAG, "Audio Buffer allocation fail: audioBuffer[0]=%p, [1]=%p",audioBuffer[0], audioBuffer[1]);
        abort();
    }
        

    gbc_sound_reset();

    lcd_begin();

    uint startTime;
    uint stopTime;
    uint totalElapsedTime = 0;
    uint actualFrameCount = 0;

    while(1){
        
        startTime = xthal_get_ccount();
        run_to_vblank();
        stopTime = xthal_get_ccount();

        input_set();

        if (stopTime > startTime)
            elapsedTime = (stopTime - startTime);
        else
            elapsedTime = ((uint64_t)stopTime + (uint64_t)0xffffffff) - (startTime);

        totalElapsedTime += elapsedTime;
        ++frame;
        ++actualFrameCount;

        if (actualFrameCount == 60)
        {
            float seconds = totalElapsedTime / (CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ * 1000000.0f); // 240000000.0f; // (240Mhz)
            float fps = actualFrameCount / seconds;

            printf("HEAP:%i, FPS:%f\n", esp_get_free_heap_size(), fps);

            actualFrameCount = 0;
            totalElapsedTime = 0;
        }
        
    }
}


static void run_to_vblank()
{
    // FRAME BEGIN 

   //  FIXME: djudging by the time specified this was intended
  //to emulate through vblank phase which is handled at the
  //end of the loop. 
    cpu_emulate(32832);

    // FIXME: R_LY >= 0; comparsion to zero can also be removed
  //altogether, R_LY is always 0 at this point 
    while (R_LY > 0 && R_LY < 144)
    {
        // Step through visible line scanning phase 
        emu_step();
    }

    // VBLANK BEGIN 

    //vid_end();
    if ((frame % 2) == 0)
    {
        xQueueSend(vidQueue, &framebuffer, 0);

        // swap buffers
        currentBuffer = currentBuffer ? 0 : 1;
        framebuffer = displayBuffer[currentBuffer];

        fb.ptr = framebuffer;
    }

    rtc_tick();

    sound_mix();

    if (pcm.pos > 100)
    {
        currentAudioBufferPtr = audioBuffer[currentAudioBuffer];
        currentAudioSampleCount = pcm.pos;

        void *tempPtr = 0x1234;
        xQueueSend(audioQueue, &tempPtr, portMAX_DELAY);

        // Swap buffers
        currentAudioBuffer = currentAudioBuffer ? 0 : 1;
        pcm.buf = audioBuffer[currentAudioBuffer];
        pcm.pos = 0;
    }

    if (!(R_LCDC & 0x80))
    {
        // LCDC operation stopped 
        // FIXME: djudging by the time specified, this is
   // intended to emulate through visible line scanning
   // phase, even though we are already at vblank here 
        cpu_emulate(32832);
    }

    while (R_LY > 0)
    {
        // Step through vblank phase 
        emu_step();
    }
}

static void input_set(){

    uint16_t inputs_value =  input_read();

    pad_set(PAD_DOWN,!((inputs_value >> 0) & 0x01));
    pad_set(PAD_LEFT,!((inputs_value >> 1) & 0x01));
    pad_set(PAD_UP,!((inputs_value >> 2) & 0x01));
    pad_set(PAD_RIGHT,!((inputs_value >> 3) & 0x01));
    pad_set(PAD_B,!((inputs_value >> 8) & 0x01));
    pad_set(PAD_A,!((inputs_value >> 9) & 0x01));
    pad_set(PAD_START,!((inputs_value >> 10) & 0x01));
    pad_set(PAD_SELECT,!((inputs_value >> 12) & 0x01));

}

