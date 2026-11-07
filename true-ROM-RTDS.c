/* ====================================================================
 * Projekt: true-ROM-RTDS (Real-Time ROM Emulator)
 * Platforma docelowa: Raspberry Pi Pico 2 (RP2350)
 * 
 * "Stworzono z pomocą modelu AI Gemini (wersja Experimental 2026) 
 *  – synergia pomysłu z potęgą maszyny."
 * ==================================================================== */


#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"

// Wymagane przez kompilator, aby dołączyć automatycznie wygenerowany kod z pliku .pio
#include "rom_emulator.pio.h"

// Definicje pinów GPIO zgodnie z naszym planem sprzętowym
#define ADDR_PIN_BASE 0  // GPIO 0 do 15 to szyna adresowa (16 pinów)
#define DATA_PIN_BASE 16 // GPIO 16 do 23 to szyna danych (8 pinów)
#define CE_PIN 24        // GPIO 24 to sygnał kontrolny /CE ze starego komputera

// Definicja wbudowanej diody LED (na standardowym Pico to GPIO 25)
const uint LED_PIN = PICO_DEFAULT_LED_PIN;

// --- FUNDAMENT DOWN-TOP: TABLICA INDEKSOWANA W SRAM ---
// Alokujemy dokładnie 64 KB pamięci. Atrybut 'alignas(65536)' to kluczowy detal.
// Zmusza on kompilator, aby adres początkowy tej tablicy w pamięci RAM mikrokontrolera
// miał same zera na najniższych 16 bitach (np. 0x20000000). Dzięki temu adres ze starego
// komputera (0x0000 - 0xFFFF) dodany do adresu bazy od razu wskazuje właściwy bajt w krzemie.

// alignas(65536) uint8_t rom_content[65536];
// alignas(65536) powodowało bład kompilacji ( main.c - byc moze przejdzie w main.ccp)
// alternatywna deklaracja tablicy
uint8_t __attribute__((aligned(65536))) rom_content[65536];

void init_emulator_dma_and_pio() {
    // Wybieramy pierwszy dostępny blok PIO (pio0)
    PIO pio = pio0;
    
    // Szukamy wolnej maszyny stanowej w tym bloku
    uint sm = pio_claim_unused_sm(pio, true);
    
    // Ładujemy nasz program z assemblera PIO do pamięci instrukcji bloku PIO
    uint offset = pio_add_program(pio, &rom_emulator_program);

    // Wywołujemy funkcję inicjalizującą piny, którą dopisaliśmy na dole pliku .pio
    rom_emulator_program_init(pio, sm, offset, ADDR_PIN_BASE, DATA_PIN_BASE);

    // --- WSTĘPNE UZBROJENIE REJESTRU KIERUNKU (DLA FUNKTORA LOGICZNEGO PIO) ---
    // Aby instrukcja 'mov pindirs, osr' w PIO mogła włączać tryb wyjścia dla danych,
    // musimy fizycznie wbić same jedynki (0xFFFFFFFF) do rejestru OSR maszyny stanowej.
    pio_sm_put_blocking(pio, sm, 0xFFFFFFFF);
    pio_sm_exec(pio, sm, pio_encode_pull(false, false));

    // --- KONFIGURACJA SEKWENCJI SPRZĘTOWEJ DMA ---
    // Rezerwujemy dwa kanały DMA, które będą przekazywać dane bez udziału procesora
    int dma_chan_addr = dma_claim_unused_channel(true);
    int dma_chan_data = dma_claim_unused_channel(true);

    // KANAŁ 1: Odczyt adresu z PIO i konfiguracja Kanału 2
    dma_channel_config c_addr = dma_channel_get_default_config(dma_chan_addr);
    channel_config_set_transfer_data_size(&c_addr, DMA_SIZE_32);
    channel_config_set_read_increment(&c_addr, false);  // Zawsze czytaj z tego samego FIFO PIO
    channel_config_set_write_increment(&c_addr, false); // Zawsze pisz do tego samego rejestru adresowego Kanału 2

    dma_channel_configure(
        dma_chan_addr,
        &c_addr,
        &dma_hw->ch[dma_chan_data].read_addr, // Cel: Rejestr adresu źródłowego Kanału 2
        &pio->rxf[sm],                        // Źródło: Kolejka FIFO z adresem od PIO
        1,                                    // Wykonaj dokładnie 1 transfer (przepisz 1 adres)
        false                                 // Nie uruchamiaj jeszcze
    );

    // KANAŁ 2: Pobranie bajtu z SRAM i wysłanie do PIO
    dma_channel_config c_data = dma_channel_get_default_config(dma_chan_data);
    channel_config_set_transfer_data_size(&c_data, DMA_SIZE_8);
    channel_config_set_read_increment(&c_data, false);  // Adres bazowy modyfikujemy sprzętowo, więc false
    channel_config_set_write_increment(&c_data, false); // Zawsze pisz do tego samego FIFO nadawczego PIO
    channel_config_set_dreq(&c_data, pio_get_dreq(pio, sm, true)); // Synchronizacja tempem automatu PIO
    
    // Magia zapętlenia: po wysłaniu bajtu, Kanał 2 automatycznie ponownie uzbraja Kanał 1
    channel_config_set_chain_to(&c_data, dma_chan_addr); 

    dma_channel_configure(
        dma_chan_data,
        &c_data,
        &pio->txf[sm],                        // Cel: FIFO nadawcze PIO (piny danych)
        rom_content,                          // Adres źródłowy: Nasza tablica EPROM w SRAM
        1,                                    // Wykonaj 1 transfer (prześlij 1 bajt)
        false                                 // Nie uruchamiaj jeszcze
    );

    // Odpalamy pierwszy bieg automatyki sprzętowej
    dma_channel_start(dma_chan_addr);
    pio_sm_set_enabled(pio, sm, true);
}

int main() {
    // Inicjalizacja standardowego zestawu bibliotek Pico SDK
    stdio_init_all();

    // Inicjalizacja pinu wbudowanej diody LED jako wyjścia
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // Wypełniamy naszą tablicę testowymi danymi (np. samymi zerami na start)
    for (int i = 0; i < 65536; i++) {
        rom_content[i] = 0x00;
    }
    // Wpisujemy "znak życia" pod pierwszy adres (Wektor resetu / początek kodu)
    rom_content[0x0000] = 0x12; 

    // Uruchamiamy autonomiczną automatykę peryferyjną (PIO + DMA)
    init_emulator_dma_and_pio();

    // Główna pętla testowa (Nasze "koronki" diagnostyczne na czas testu środowiska)
    // Ponieważ automatyka działa całkowicie w tle na poziomie rejestrów krzemowych,
    // procesor w tej pętli może bez przeszkód mrugać diodą i pisać na USB!
    while (true) {
        // Włącz diodę LED
        gpio_put(LED_PIN, 1);
        
        // Wyślij tekst diagnostyczny przez USB na Twój laptop
        printf("TRUE ROM EMULATOR: Jadro systemu dziala. Automatyka PIO+DMA aktywna.\r\n");
        
        // Odczekaj 500 milisekund
        sleep_ms(500);

        // Wyłącz diodę LED i odczekaj kolejne 500 ms
        gpio_put(LED_PIN, 0);
        sleep_ms(500);
    }

    return 0;
}
