#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#define ARM_MATH_CM0PLUS
#include <arm_math.h>
#include "hardware/adc.h"
#include "hardware/dma.h"

// === PANTALLA ===
#define TFT_CS   17
#define TFT_DC   15
#define TFT_RST  -1
Adafruit_ILI9341 display(TFT_CS, TFT_DC, TFT_RST);

// === ADC + DMA ===
#define AUDIO_ADC_PIN 26     // GPIO26 = ADC0
const uint16_t FFT_N      = 1024;
const int16_t  ADC_OFFSET = 2048;  // bias 12-bit en 0…4095
static uint16_t capture_buf[FFT_N];
int dma_chan;

// === FFT buffers ===
static float32_t hammingWindow[FFT_N];
static float32_t fftOutput[FFT_N];
static float32_t magnitudes[FFT_N/2 + 1];
static arm_rfft_fast_instance_f32 fftInst;

// === GRÁFICO ===
const int WIDTH   = 320;
const int HEIGHT  = 240;
const int X0      = 0;
const int Y0      = 20;
const int GRAPH_H = HEIGHT - Y0 - 15;
static int16_t lastHeight[WIDTH];
static float   peakMag = 1.0f;
const uint8_t  BAR_DECAY = 2;

// Frecuencia de muestreo deseada
const float SAMPLE_FREQ = 48000.0f;

// Genera ventana Hamming
void generateHamming() {
  for (uint16_t i = 0; i < FFT_N; i++) {
    hammingWindow[i] = 0.54f - 0.46f * arm_cos_f32(2*PI*i/(FFT_N-1));
  }
}

// Dibuja el eje con SAMPLE_FREQ
void drawFrequencyAxis() {
  float nyquist = SAMPLE_FREQ/2;
  display.drawFastHLine(X0, Y0+GRAPH_H, WIDTH, ILI9341_WHITE);
  display.setTextColor(ILI9341_WHITE);
  display.setTextSize(1);
  for (int khz=0; khz<=int(nyquist/1000); khz+=2) {
    float fx = khz*1000.0f;
    int x = X0 + int((fx/nyquist)*(WIDTH-1) + 0.5f);
    display.drawFastVLine(x, Y0+GRAPH_H, 5, ILI9341_WHITE);
    if(khz != 0)
      display.setCursor(x-8, Y0+GRAPH_H+6);
    else
      display.setCursor(x, Y0+GRAPH_H+6);
    display.print(khz);
    display.print('k');   
  }
}

void setup() {
  Serial.begin(115200);
  // TFT
  SPI.begin();
  display.begin();
  display.setRotation(3);
  display.fillScreen(ILI9341_BLACK);
  pinMode(LED_BUILTIN, OUTPUT);

  // --- Inicializar ADC ---
  adc_init();
  adc_gpio_init(AUDIO_ADC_PIN);
  adc_select_input(0);            // canal 0 → GPIO26
  // FIFO + DREQ (1 muestra → petición DMA)
  adc_fifo_setup(true, true, 1, false, false);
  // Divisor para ~48 kHz (48 MHz/48 k = 1 000 ciclos/muestra)
  adc_set_clkdiv((48000000.0f / SAMPLE_FREQ) - 1);
  adc_run(true);

  // --- Configurar DMA ---
  dma_chan = dma_claim_unused_channel(true);
  dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
  channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
  channel_config_set_read_increment(&cfg, false);
  channel_config_set_write_increment(&cfg, true);
  channel_config_set_dreq(&cfg, DREQ_ADC);
  dma_channel_configure(dma_chan, &cfg,
                        capture_buf,           // dst
                        &adc_hw->fifo,         // src
                        FFT_N,                 // count
                        true                   // start now
  );

  // --- FFT & ventana ---
  arm_rfft_fast_init_f32(&fftInst, FFT_N);
  generateHamming();

  // --- Pantalla inicial ---
  display.setTextColor(ILI9341_WHITE);
  display.setTextSize(1);
  display.setCursor(1,5);
  display.print("Luis Francisco Sanchez");
  display.setCursor(140,5);
  display.printf("Fs:");
  display.setTextColor(ILI9341_GREEN);
  display.printf("%.1f kHz ", SAMPLE_FREQ/1000.0f);
  display.setTextColor(ILI9341_WHITE);
  display.printf("Muestras FFT:");
  display.setTextColor(ILI9341_GREEN);
  display.printf("%d", FFT_N);
  display.fillRect(X0, Y0, WIDTH, GRAPH_H, ILI9341_BLACK);
  drawFrequencyAxis();
  memset(lastHeight, 0, sizeof(lastHeight));
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);

  // 1) Esperar a que el DMA complete las muestras
  dma_channel_wait_for_finish_blocking(dma_chan);
  adc_run(false);

  // 2) Preparar fftInput…
  float32_t fftInput[FFT_N];
  for (uint16_t i = 0; i < FFT_N; i++) {
    fftInput[i] = float32_t(capture_buf[i]) - ADC_OFFSET;
    fftInput[i] *= hammingWindow[i];
  }

  // 3) FFT
  arm_rfft_fast_f32(&fftInst, fftInput, fftOutput, 0);

  // 4) Magnitudes y cálculo de peak…
  magnitudes[0]       = fabsf(fftOutput[0]);
  magnitudes[FFT_N/2] = fabsf(fftOutput[1]);
  float currentPeak = max(magnitudes[0], magnitudes[FFT_N/2]);
  for (uint16_t k = 1; k < FFT_N/2; k++) {
    float re = fftOutput[2*k];
    float im = fftOutput[2*k+1];
    magnitudes[k] = sqrtf(re*re + im*im);
    if (magnitudes[k] > currentPeak) currentPeak = magnitudes[k];
  }
  peakMag = max(peakMag * 0.995f, currentPeak);
  if (peakMag < 1.0f) peakMag = 1.0f;

  // ———————————————
  // 5) **LIMPIAR Y REDIBUJAR TODO**
  // Borramos la zona del gráfico
  display.fillRect(X0, Y0, WIDTH, GRAPH_H, ILI9341_BLACK);
  // Volvemos a dibujar el eje de frecuencias (línea y marcas)
  drawFrequencyAxis();

  // 6) Dibujar **todas** las barras desde cero
  for (int x = 2; x < WIDTH; x++) {
    uint16_t bin = uint32_t(x) * (FFT_N/2) / (WIDTH-1);
    int h = int((magnitudes[bin] / peakMag) * GRAPH_H);
    h = constrain(h, 0, GRAPH_H);
    // Línea vertical con la altura exacta
    display.drawFastVLine(X0 + x, Y0 + GRAPH_H - h, h, ILI9341_CYAN);
  }

  // 7) Rearmar DMA + ADC
  adc_fifo_drain();
  dma_channel_set_read_addr(dma_chan, &adc_hw->fifo, false);
  dma_channel_set_write_addr(dma_chan, capture_buf, false);
  dma_channel_set_trans_count(dma_chan, FFT_N, false);
  dma_channel_start(dma_chan);
  adc_run(true);

  digitalWrite(LED_BUILTIN, LOW);
}



