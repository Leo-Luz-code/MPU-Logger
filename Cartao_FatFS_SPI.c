#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "ssd1306.h"
#include "font.h"

#include "hardware/adc.h"
#include "hardware/rtc.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "ff.h"
#include "diskio.h"
#include "f_util.h"
#include "hw_config.h"
#include "my_debug.h"
#include "rtc.h"
#include "sd_card.h"

#define I2C_PORT i2c0
#define I2C_SDA 0
#define I2C_SCL 1
#define I2C_PORT_DISP i2c1
#define I2C_SDA_DISP 14
#define I2C_SCL_DISP 15
#define ENDERECO_DISP 0x3C
#define DISP_W 128
#define DISP_H 64
#define BOTAO_A 5
#define BOTAO_B 6
#define LED_GREEN_PIN 11
#define LED_BLUE_PIN 12
#define LED_RED_PIN 13

static int addr = 0x68;

typedef enum
{
    INITIALIZING,
    WAITING,
    RECORDING,
    MOUNTING,
    UNMOUNTING
} SystemState;

volatile SystemState currentState = INITIALIZING;

static bool logger_enabled;
static const uint32_t period = 1000;
static absolute_time_t next_log_time;

static volatile uint32_t current_time; // Tempo atual (usado para debounce)
static volatile uint32_t last_time_button = 0;

static volatile bool is_mounted = false;

ssd1306_t ssd;

// Funções para controle de LEDs
void led_blue(bool state);
void led_green(bool state);
void led_red(bool state);
void led_white(void);
void led_yellow(void);
void led_purple(void);
void led_cyan(void);
void led_off(void);

/**
 * @brief Encontra um nome de arquivo único no formato "base<numero>.csv".
 * * @param buffer O buffer de char onde o nome do arquivo será armazenado.
 * @param buffer_size O tamanho do buffer.
 * @param base_name A base do nome do arquivo (ex: "mpu_data").
 */
void find_unique_filename(char *buffer, size_t buffer_size, const char *base_name)
{
    int file_index = 1;
    FILINFO fno; // Estrutura para receber informações do arquivo

    // Loop para encontrar um nome de arquivo que não exista
    while (true)
    {
        // Cria o nome do arquivo com o índice atual (ex: "mpu_data1.csv")
        snprintf(buffer, buffer_size, "%s%d.csv", base_name, file_index);

        // Verifica se o arquivo já existe
        FRESULT fr = f_stat(buffer, &fno);

        if (fr == FR_NO_FILE)
        {
            // Se o arquivo não existe, encontramos um nome válido.
            printf("\nNome de arquivo disponivel: %s\n", buffer);
            break; // Sai do loop
        }
        else if (fr == FR_OK)
        {
            // O arquivo existe, então tentamos o próximo número.
            printf("\nArquivo '%s' ja existe. Tentando proximo...\n", buffer);
            file_index++;
        }
        else
        {
            // Ocorreu um erro inesperado (ex: cartão SD removido)
            printf("[ERRO] f_stat falhou com codigo: %d\n", fr);
            // Como fallback, para e tenta usar o último nome gerado
            break;
        }
    }
}

static void mpu6050_reset()
{
    uint8_t buf[] = {0x6B, 0x80};
    i2c_write_blocking(I2C_PORT, addr, buf, 2, false);
    sleep_ms(100);
    buf[1] = 0x00;
    i2c_write_blocking(I2C_PORT, addr, buf, 2, false);
    sleep_ms(10);
}

static void mpu6050_read_raw(int16_t accel[3], int16_t gyro[3], int16_t *temp)
{
    uint8_t buffer[6];
    uint8_t val = 0x3B;
    i2c_write_blocking(I2C_PORT, addr, &val, 1, true);
    i2c_read_blocking(I2C_PORT, addr, buffer, 6, false);
    for (int i = 0; i < 3; i++)
        accel[i] = (buffer[i * 2] << 8) | buffer[(i * 2) + 1];

    val = 0x43;
    i2c_write_blocking(I2C_PORT, addr, &val, 1, true);
    i2c_read_blocking(I2C_PORT, addr, buffer, 6, false);
    for (int i = 0; i < 3; i++)
        gyro[i] = (buffer[i * 2] << 8) | buffer[(i * 2) + 1];

    val = 0x41;
    i2c_write_blocking(I2C_PORT, addr, &val, 1, true);
    i2c_read_blocking(I2C_PORT, addr, buffer, 2, false);
    *temp = (buffer[0] << 8) | buffer[1];
}

static sd_card_t *sd_get_by_name(const char *const name)
{
    for (size_t i = 0; i < sd_get_num(); ++i)
        if (0 == strcmp(sd_get_by_num(i)->pcName, name))
            return sd_get_by_num(i);
    DBG_PRINTF("%s: unknown name %s\n", __func__, name);
    return NULL;
}
static FATFS *sd_get_fs_by_name(const char *name)
{
    for (size_t i = 0; i < sd_get_num(); ++i)
        if (0 == strcmp(sd_get_by_num(i)->pcName, name))
            return &sd_get_by_num(i)->fatfs;
    DBG_PRINTF("%s: unknown name %s\n", __func__, name);
    return NULL;
}

static void run_setrtc()
{
    const char *dateStr = strtok(NULL, " ");
    if (!dateStr)
    {
        printf("Missing argument\n");
        return;
    }
    int date = atoi(dateStr);

    const char *monthStr = strtok(NULL, " ");
    if (!monthStr)
    {
        printf("Missing argument\n");
        return;
    }
    int month = atoi(monthStr);

    const char *yearStr = strtok(NULL, " ");
    if (!yearStr)
    {
        printf("Missing argument\n");
        return;
    }
    int year = atoi(yearStr) + 2000;

    const char *hourStr = strtok(NULL, " ");
    if (!hourStr)
    {
        printf("Missing argument\n");
        return;
    }
    int hour = atoi(hourStr);

    const char *minStr = strtok(NULL, " ");
    if (!minStr)
    {
        printf("Missing argument\n");
        return;
    }
    int min = atoi(minStr);

    const char *secStr = strtok(NULL, " ");
    if (!secStr)
    {
        printf("Missing argument\n");
        return;
    }
    int sec = atoi(secStr);

    datetime_t t = {
        .year = (int16_t)year,
        .month = (int8_t)month,
        .day = (int8_t)date,
        .dotw = 0, // 0 is Sunday
        .hour = (int8_t)hour,
        .min = (int8_t)min,
        .sec = (int8_t)sec};
    rtc_set_datetime(&t);
}

static void run_format()
{
    const char *arg1 = strtok(NULL, " ");
    if (!arg1)
        arg1 = sd_get_by_num(0)->pcName;
    FATFS *p_fs = sd_get_fs_by_name(arg1);
    if (!p_fs)
    {
        printf("Unknown logical drive number: \"%s\"\n", arg1);
        return;
    }
    /* Format the drive with default parameters */
    FRESULT fr = f_mkfs(arg1, 0, 0, FF_MAX_SS * 2);
    if (FR_OK != fr)
        printf("f_mkfs error: %s (%d)\n", FRESULT_str(fr), fr);
}
static void run_mount()
{
    const char *arg1 = strtok(NULL, " ");
    if (!arg1)
        arg1 = sd_get_by_num(0)->pcName;
    FATFS *p_fs = sd_get_fs_by_name(arg1);
    if (!p_fs)
    {
        printf("Unknown logical drive number: \"%s\"\n", arg1);
        return;
    }
    FRESULT fr = f_mount(p_fs, arg1, 1);
    if (FR_OK != fr)
    {
        printf("f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
        return;
    }
    sd_card_t *pSD = sd_get_by_name(arg1);
    myASSERT(pSD);
    pSD->mounted = true;
    is_mounted = true;
    printf("\nProcesso de montagem do SD ( %s ) concluído\n", pSD->pcName);
}
static void run_unmount()
{
    const char *arg1 = strtok(NULL, " ");
    if (!arg1)
        arg1 = sd_get_by_num(0)->pcName;
    FATFS *p_fs = sd_get_fs_by_name(arg1);
    if (!p_fs)
    {
        printf("Unknown logical drive number: \"%s\"\n", arg1);
        return;
    }
    FRESULT fr = f_unmount(arg1);
    if (FR_OK != fr)
    {
        printf("f_unmount error: %s (%d)\n", FRESULT_str(fr), fr);
        return;
    }
    sd_card_t *pSD = sd_get_by_name(arg1);
    myASSERT(pSD);
    pSD->mounted = false;
    is_mounted = false;
    pSD->m_Status |= STA_NOINIT; // in case medium is removed
    printf("\nSD ( %s ) desmontado\n", pSD->pcName);
}
static void run_getfree()
{
    const char *arg1 = strtok(NULL, " ");
    if (!arg1)
        arg1 = sd_get_by_num(0)->pcName;
    DWORD fre_clust, fre_sect, tot_sect;
    FATFS *p_fs = sd_get_fs_by_name(arg1);
    if (!p_fs)
    {
        printf("Unknown logical drive number: \"%s\"\n", arg1);
        return;
    }
    FRESULT fr = f_getfree(arg1, &fre_clust, &p_fs);
    if (FR_OK != fr)
    {
        printf("f_getfree error: %s (%d)\n", FRESULT_str(fr), fr);
        return;
    }
    tot_sect = (p_fs->n_fatent - 2) * p_fs->csize;
    fre_sect = fre_clust * p_fs->csize;
    printf("%10lu KiB total drive space.\n%10lu KiB available.\n", tot_sect / 2, fre_sect / 2);
}

static void run_ls()
{
    const char *arg1 = strtok(NULL, " ");
    if (!arg1)
        arg1 = "";
    char cwdbuf[FF_LFN_BUF] = {0};
    FRESULT fr;
    char const *p_dir;
    if (arg1[0])
    {
        p_dir = arg1;
    }
    else
    {
        fr = f_getcwd(cwdbuf, sizeof cwdbuf);
        if (FR_OK != fr)
        {
            printf("f_getcwd error: %s (%d)\n", FRESULT_str(fr), fr);
            return;
        }
        p_dir = cwdbuf;
    }
    printf("Directory Listing: %s\n", p_dir);
    DIR dj;
    FILINFO fno;
    memset(&dj, 0, sizeof dj);
    memset(&fno, 0, sizeof fno);
    fr = f_findfirst(&dj, &fno, p_dir, "*");
    if (FR_OK != fr)
    {
        printf("f_findfirst error: %s (%d)\n", FRESULT_str(fr), fr);
        return;
    }
    while (fr == FR_OK && fno.fname[0])
    {
        const char *pcWritableFile = "writable file",
                   *pcReadOnlyFile = "read only file",
                   *pcDirectory = "directory";
        const char *pcAttrib;
        if (fno.fattrib & AM_DIR)
            pcAttrib = pcDirectory;
        else if (fno.fattrib & AM_RDO)
            pcAttrib = pcReadOnlyFile;
        else
            pcAttrib = pcWritableFile;
        printf("%s [%s] [size=%llu]\n", fno.fname, pcAttrib, fno.fsize);

        fr = f_findnext(&dj, &fno);
    }
    f_closedir(&dj);
}
// Função para ler o conteúdo de um arquivo e exibir no terminal
void read_file(const char *filename)
{
    FIL file;
    FRESULT res = f_open(&file, filename, FA_READ);
    if (res != FR_OK)
    {
        printf("[ERRO] Não foi possível abrir o arquivo para leitura. Verifique se o Cartão está montado ou se o arquivo existe.\n");

        return;
    }
    char buffer[128];
    UINT br;
    printf("Conteúdo do arquivo %s:\n", filename);
    while (f_read(&file, buffer, sizeof(buffer) - 1, &br) == FR_OK && br > 0)
    {
        buffer[br] = '\0';
        printf("%s", buffer);
    }
    f_close(&file);
    printf("\nLeitura do arquivo %s concluída.\n\n", filename);
}

static void read_all_files()
{
    const char *arg1 = strtok(NULL, " ");
    if (!arg1)
        arg1 = "";
    char cwdbuf[FF_LFN_BUF] = {0};
    FRESULT fr;
    char const *p_dir;
    if (arg1[0])
    {
        p_dir = arg1;
    }
    else
    {
        fr = f_getcwd(cwdbuf, sizeof cwdbuf);
        if (FR_OK != fr)
        {
            printf("f_getcwd error: %s (%d)\n", FRESULT_str(fr), fr);
            return;
        }
        p_dir = cwdbuf;
    }
    printf("Directory Listing: %s\n", p_dir);
    DIR dj;
    FILINFO fno;
    memset(&dj, 0, sizeof dj);
    memset(&fno, 0, sizeof fno);
    fr = f_findfirst(&dj, &fno, p_dir, "*");
    if (FR_OK != fr)
    {
        printf("f_findfirst error: %s (%d)\n", FRESULT_str(fr), fr);
        return;
    }
    while (fr == FR_OK && fno.fname[0])
    {
        const char *pcWritableFile = "writable file",
                   *pcReadOnlyFile = "read only file",
                   *pcDirectory = "directory";
        const char *pcAttrib;
        if (fno.fattrib & AM_DIR)
            pcAttrib = pcDirectory;
        else if (fno.fattrib & AM_RDO)
            pcAttrib = pcReadOnlyFile;
        else
            pcAttrib = pcWritableFile;
        printf("%s [%s] [size=%llu]\n", fno.fname, pcAttrib, fno.fsize);

        read_file(fno.fname);

        fr = f_findnext(&dj, &fno);
    }
    f_closedir(&dj);
}

static void run_cat()
{
    char *arg1 = strtok(NULL, " ");
    if (!arg1)
    {
        printf("Missing argument\n");
        return;
    }
    FIL fil;
    FRESULT fr = f_open(&fil, arg1, FA_READ);
    if (FR_OK != fr)
    {
        printf("f_open error: %s (%d)\n", FRESULT_str(fr), fr);
        return;
    }
    char buf[256];
    while (f_gets(buf, sizeof buf, &fil))
    {
        printf("%s", buf);
    }
    fr = f_close(&fil);
    if (FR_OK != fr)
        printf("f_open error: %s (%d)\n", FRESULT_str(fr), fr);
}

// Função para capturar dados do MPU e salvar no arquivo
void capture_mpu_data_and_save()
{
    // Buffer para o nome do arquivo. Será preenchido pela função find_unique_filename
    char filename[25];
    find_unique_filename(filename, sizeof(filename), "mpu_data");

    printf("\nCapturando dados do MPU. Pressione o botao para parar...\n");

    FIL file;
    // Usa a flag FA_CREATE_NEW, que cria um novo arquivo. Falha se o arquivo já existir.
    // Como nossa lógica anterior já garante que o arquivo não existe, isso é mais seguro.
    FRESULT res = f_open(&file, filename, FA_WRITE | FA_CREATE_NEW);

    if (res != FR_OK)
    {
        led_off();
        printf("\n[ERRO] Nao foi possivel criar o arquivo '%s'. Erro: %d\n", filename, res);
        ssd1306_fill(&ssd, false);
        ssd1306_draw_string(&ssd, "Erro no SD", DISP_W / 2 - 50, DISP_H / 2 - 10);
        ssd1306_draw_string(&ssd, "Voce montou?", DISP_W / 2 - 50, DISP_H / 2);

        ssd1306_send_data(&ssd);
        led_red(true);
        return;
    }

    // Escreve o cabeçalho no arquivo CSV, conforme especificado na tarefa
    f_puts("numero_amostra,accel_x,accel_y,accel_z,giro_x,giro_y,giro_z\n", &file);

    uint32_t sample_count = 0;

    // O loop continua enquanto o estado for RECORDING
    while (currentState == RECORDING)
    {
        uint32_t current_time = to_us_since_boot(get_absolute_time());
        uint32_t last_time_led = 0;

        if (current_time - last_time_led > 500000) // Pisca o LED a cada 500ms
        {
            led_purple();
            last_time_led = current_time;
        }

        int16_t aceleracao[3], gyro[3], temp;
        mpu6050_read_raw(aceleracao, gyro, &temp);
        sample_count++;

        const float accel_scale = 2.0f;  // +- 2g
        const float gyro_scale = 500.0f; // +- 500°/s

        // Normalização
        float accel_x_g = (float)aceleracao[0] * accel_scale / 32768.0f;
        float accel_y_g = (float)aceleracao[1] * accel_scale / 32768.0f;
        float accel_z_g = (float)aceleracao[2] * accel_scale / 32768.0f;

        float gyro_x_dps = (float)gyro[0] * gyro_scale / 32768.0f;
        float gyro_y_dps = (float)gyro[1] * gyro_scale / 32768.0f;
        float gyro_z_dps = (float)gyro[2] * gyro_scale / 32768.0f;

        char buffer[150];
        // Formato CSV corrigido: valores separados por vírgula, sem espaços ou texto extra
        sprintf(buffer, "%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                sample_count,
                accel_x_g, accel_y_g, accel_z_g,
                gyro_x_dps, gyro_y_dps, gyro_z_dps);

        UINT bw;
        res = f_write(&file, buffer, strlen(buffer), &bw);

        // Atualiza display com o progresso
        char display_line[20];
        sprintf(display_line, "Amostras: %lu", sample_count);
        ssd1306_fill(&ssd, false);
        ssd1306_draw_string(&ssd, display_line, DISP_W / 2 - 50, DISP_H / 2 - 10);
        ssd1306_draw_string(&ssd, "Gravando...", DISP_W / 2 - 50, DISP_H / 2 + 10);
        ssd1306_send_data(&ssd);

        if (res != FR_OK)
        {
            printf("[ERRO] Nao foi possivel escrever no arquivo.\n");
            ssd1306_fill(&ssd, false);
            ssd1306_draw_string(&ssd, "Falha ao gravar", DISP_W / 2 - 50, DISP_H / 2 - 10);
            ssd1306_send_data(&ssd);
            f_close(&file);
            return;
        }

        led_off();
        sleep_ms(100); // Frequência de amostragem
    }

    // Esta parte do código é executada quando o loop termina (currentState != RECORDING)
    f_close(&file);
    led_off();
    printf("\nCaptura interrompida. Dados salvos em %s\n", filename);
    ssd1306_fill(&ssd, false);
    ssd1306_draw_string(&ssd, "Captura", DISP_W / 2 - 50, DISP_H / 2 - 10);
    ssd1306_draw_string(&ssd, "interrompida", DISP_W / 2 - 50, DISP_H / 2);
    ssd1306_send_data(&ssd);
}

void gpio_irq_handler(uint gpio, uint32_t events)
{
    current_time = to_us_since_boot(get_absolute_time());

    if (current_time - last_time_button > 200000)
    {
        last_time_button = current_time;

        if (gpio == BOTAO_A)
            currentState = currentState == RECORDING ? WAITING : RECORDING;

        if (gpio == BOTAO_B)
            currentState = is_mounted ? UNMOUNTING : MOUNTING;
    }
}

static void run_help()
{
    printf("\nComandos disponíveis:\n\n");
    printf("Digite 'a' para montar o cartão SD\n");
    printf("Digite 'b' para desmontar o cartão SD\n");
    printf("Digite 'c' para listar arquivos\n");
    printf("Digite 'd' para mostrar conteúdo de todas as leituras\n");
    printf("Digite 'e' para obter espaço livre no cartão SD\n");
    printf("Digite 'f' para capturar dados do ADC e salvar no arquivo\n");
    printf("Digite 'g' para formatar o cartão SD\n");
    printf("Digite 'h' para exibir os comandos disponíveis\n");
    printf("\nEscolha o comando:  ");
}

typedef void (*p_fn_t)();
typedef struct
{
    char const *const command;
    p_fn_t const function;
    char const *const help;
} cmd_def_t;

static cmd_def_t cmds[] = {
    {"setrtc", run_setrtc, "setrtc <DD> <MM> <YY> <hh> <mm> <ss>: Set Real Time Clock"},
    {"format", run_format, "format [<drive#:>]: Formata o cartão SD"},
    {"mount", run_mount, "mount [<drive#:>]: Monta o cartão SD"},
    {"unmount", run_unmount, "unmount <drive#:>: Desmonta o cartão SD"},
    {"getfree", run_getfree, "getfree [<drive#:>]: Espaço livre"},
    {"ls", run_ls, "ls: Lista arquivos"},
    {"cat", run_cat, "cat <filename>: Mostra conteúdo do arquivo"},
    {"help", run_help, "help: Mostra comandos disponíveis"}};

static void process_stdio(int cRxedChar)
{
    static char cmd[256];
    static size_t ix;

    if (!isprint(cRxedChar) && !isspace(cRxedChar) && '\r' != cRxedChar &&
        '\b' != cRxedChar && cRxedChar != (char)127)
        return;
    printf("%c", cRxedChar); // echo
    stdio_flush();
    if (cRxedChar == '\r')
    {
        printf("%c", '\n');
        stdio_flush();

        if (!strnlen(cmd, sizeof cmd))
        {
            printf("> ");
            stdio_flush();
            return;
        }
        char *cmdn = strtok(cmd, " ");
        if (cmdn)
        {
            size_t i;
            for (i = 0; i < count_of(cmds); ++i)
            {
                if (0 == strcmp(cmds[i].command, cmdn))
                {
                    (*cmds[i].function)();
                    break;
                }
            }
            if (count_of(cmds) == i)
                printf("Command \"%s\" not found\n", cmdn);
        }
        ix = 0;
        memset(cmd, 0, sizeof cmd);
        printf("\n> ");
        stdio_flush();
    }
    else
    {
        if (cRxedChar == '\b' || cRxedChar == (char)127)
        {
            if (ix > 0)
            {
                ix--;
                cmd[ix] = '\0';
            }
        }
        else
        {
            if (ix < sizeof cmd - 1)
            {
                cmd[ix] = cRxedChar;
                ix++;
            }
        }
    }
}

void led_blue(bool state)
{
    gpio_put(LED_BLUE_PIN, state);
}

void led_green(bool state)
{
    gpio_put(LED_GREEN_PIN, state);
}

void led_red(bool state)
{
    gpio_put(LED_RED_PIN, state);
}

void led_white()
{
    led_red(true);
    led_green(true);
    led_blue(true);
}

void led_yellow()
{
    led_red(true);
    led_green(true);
    led_blue(false);
}

void led_purple()
{
    led_red(true);
    led_green(false);
    led_blue(true);
}

void led_cyan()
{
    led_red(false);
    led_green(true);
    led_blue(true);
}

void led_off()
{
    led_red(false);
    led_green(false);
    led_blue(false);
}

int main()
{
    // Para ser utilizado o modo BOOTSEL com botão B
    gpio_init(BOTAO_A);
    gpio_set_dir(BOTAO_A, GPIO_IN);
    gpio_pull_up(BOTAO_A);
    gpio_set_irq_enabled_with_callback(BOTAO_A, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);

    gpio_init(LED_BLUE_PIN);
    gpio_set_dir(LED_BLUE_PIN, GPIO_OUT);
    gpio_pull_up(LED_BLUE_PIN);

    gpio_init(LED_GREEN_PIN);
    gpio_set_dir(LED_GREEN_PIN, GPIO_OUT);
    gpio_pull_up(LED_GREEN_PIN);

    gpio_init(LED_RED_PIN);
    gpio_set_dir(LED_RED_PIN, GPIO_OUT);
    gpio_pull_up(LED_RED_PIN);

    gpio_init(BOTAO_B);
    gpio_set_dir(BOTAO_B, GPIO_IN);
    gpio_pull_up(BOTAO_B);
    gpio_set_irq_enabled(BOTAO_B, GPIO_IRQ_EDGE_FALL, true);

    stdio_init_all();
    sleep_ms(5000);
    time_init();
    adc_init();

    i2c_init(I2C_PORT_DISP, 400 * 1000);
    gpio_set_function(I2C_SDA_DISP, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_DISP, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_DISP);
    gpio_pull_up(I2C_SCL_DISP);

    ssd1306_init(&ssd, DISP_W, DISP_H, false, ENDERECO_DISP, I2C_PORT_DISP);
    ssd1306_config(&ssd);

    ssd1306_fill(&ssd, false);

    if (currentState == INITIALIZING)
    {
        ssd1306_draw_string(&ssd, "Inicializando", DISP_W / 2 - 50, DISP_H / 2 - 10);
        ssd1306_draw_string(&ssd, "o sistema...", DISP_W / 2 - 50, DISP_H / 2);
        ssd1306_send_data(&ssd);
        led_yellow();
    }

    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    bi_decl(bi_2pins_with_func(I2C_SDA, I2C_SCL, GPIO_FUNC_I2C));
    mpu6050_reset();

    sleep_ms(3000); // Aguarda a inicialização do MPU6050

    if (currentState == INITIALIZING)
    {
        ssd1306_fill(&ssd, false);
        ssd1306_draw_string(&ssd, "Inicializado!", DISP_W / 2 - 50, DISP_H / 2 - 10);
        ssd1306_send_data(&ssd);
        sleep_ms(2000);
        currentState = WAITING;
    }

    printf("FatFS SPI example\n");
    printf("\033[2J\033[H"); // Limpa tela
    printf("\n> ");
    stdio_flush();
    //    printf("A tela foi limpa...\n");
    //    printf("Depois do Flush\n");
    run_help();
    led_off();
    while (true)
    {
        switch (currentState)
        {
        case WAITING:
            int cRxedChar = getchar_timeout_us(0);
            if (PICO_ERROR_TIMEOUT != cRxedChar)
                process_stdio(cRxedChar);

            if (cRxedChar == 'a') // Monta o SD card se pressionar 'a'
            {
                currentState = MOUNTING;
            }
            if (cRxedChar == 'b') // Desmonta o SD card se pressionar 'b'
            {
                currentState = UNMOUNTING;
            }
            if (cRxedChar == 'c') // Lista diretórios e os arquivos se pressionar 'c'
            {
                printf("\nListagem de arquivos no cartão SD.\n");
                run_ls();
                printf("\nListagem concluída.\n");
                printf("\nEscolha o comando (h = help):  ");
            }
            if (cRxedChar == 'd') // Exibe o conteúdo do arquivo se pressionar 'd'
            {
                read_all_files();
                printf("Escolha o comando (h = help):  ");
            }
            if (cRxedChar == 'e') // Obtém o espaço livre no SD card se pressionar 'e'
            {
                printf("\nObtendo espaço livre no SD.\n\n");
                run_getfree();
                printf("\nEspaço livre obtido.\n");
                printf("\nEscolha o comando (h = help):  ");
            }
            if (cRxedChar == 'f') // Muda o status para RECORDING
            {
                currentState = RECORDING;
            }
            if (cRxedChar == 'g') // Formata o SD card se pressionar 'g'
            {
                printf("\nProcesso de formatação do SD iniciado. Aguarde...\n");
                run_format();
                printf("\nFormatação concluída.\n\n");
                printf("\nEscolha o comando (h = help):  ");
            }
            if (cRxedChar == 'h') // Exibe os comandos disponíveis se pressionar 'h'
            {
                run_help();
            }

            ssd1306_fill(&ssd, false);
            ssd1306_draw_string(&ssd, "Aguardando...", DISP_W / 2 - 50, DISP_H / 2 + 10);
            ssd1306_draw_string(&ssd, "BA: Capturar", 0, 5);
            ssd1306_draw_string(&ssd, is_mounted ? "BB: Desmontar" : "BB: Montar", 0, 15);
            ssd1306_send_data(&ssd);

            if (!gpio_get(LED_GREEN_PIN))
            {
                led_green(true);
                led_red(false);
                led_blue(false);
            }

            sleep_ms(500);
            break;

        case RECORDING:
            ssd1306_fill(&ssd, false);
            led_purple();
            ssd1306_draw_string(&ssd, "Iniciando", DISP_W / 2 - 50, DISP_H / 2 - 10);
            ssd1306_draw_string(&ssd, "captura...", DISP_W / 2 - 50, DISP_H / 2);
            ssd1306_send_data(&ssd);
            sleep_ms(2000);
            ssd1306_send_data(&ssd);
            capture_mpu_data_and_save();
            sleep_ms(2000);
            currentState = WAITING; // Volta para o estado de espera após gravar os dados
            printf("\nEscolha o comando (h = help):  ");
            break;

        case MOUNTING:
            led_cyan();
            ssd1306_fill(&ssd, false);
            ssd1306_draw_string(&ssd, "Montando", DISP_W / 2 - 50, DISP_H / 2 - 10);
            ssd1306_draw_string(&ssd, "cartao SD...", DISP_W / 2 - 50, DISP_H / 2);
            ssd1306_send_data(&ssd);
            sleep_ms(2000); // Simula o tempo de montagem
            run_mount();
            ssd1306_fill(&ssd, false);
            ssd1306_draw_string(&ssd, "Cartao SD", DISP_W / 2 - 50, DISP_H / 2 - 10);
            ssd1306_draw_string(&ssd, "montado!", DISP_W / 2 - 50, DISP_H / 2);
            ssd1306_send_data(&ssd);
            sleep_ms(2000);         // Simula o tempo de montagem
            currentState = WAITING; // Volta para o estado de espera após montar o SD
            printf("\nEscolha o comando (h = help):  ");
            led_off();
            break;

        case UNMOUNTING:
            led_off();
            led_blue(true);
            ssd1306_fill(&ssd, false);
            ssd1306_draw_string(&ssd, "Desmontando", DISP_W / 2 - 50, DISP_H / 2 - 10);
            ssd1306_draw_string(&ssd, "cartao SD...", DISP_W / 2 - 50, DISP_H / 2);
            ssd1306_send_data(&ssd);
            sleep_ms(2000); // Simula o tempo de desmontagem
            run_unmount();
            ssd1306_fill(&ssd, false);
            ssd1306_draw_string(&ssd, "Cartao SD", DISP_W / 2 - 50, DISP_H / 2 - 10);
            ssd1306_draw_string(&ssd, "desmontado!", DISP_W / 2 - 50, DISP_H / 2);
            ssd1306_send_data(&ssd);
            sleep_ms(2000);         // Simula o tempo de montagem
            currentState = WAITING; // Volta para o estado de espera após desmontar o SD
            printf("\nEscolha o comando (h = help):  ");
            led_off();
            break;

        default:
            break;
        }
    }
    return 0;
}