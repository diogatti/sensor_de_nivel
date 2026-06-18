#include <stdio.h>
#include <math.h>

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include <stdlib.h>
#include <string.h>
// ==========================
// CONFIGURAÇÕES
// ==========================

// deixar o serial monitor o line ending em CR para poder alterar os valores
// CASO A BOMBA FIQUE TENTANDO ENCHER MAIS QUE 30 S O SISTEMA DETECTA VAZAMENTO E PARA O CODIGO, PARA REINICIAR DIGITE RESET
// VALOR DE NIVEL DESEJADO É POSSIVEL ALTERAR DIGITAR SOMENTE O VALOR NO SERIAL MONITOR 
#define ADC_PIN         26      // ADC0
#define ADC_CHANNEL     0

#define IN1             14
#define IN2             15

#define I2C_PORT i2c0
#define SDA_PIN         8
#define SCL_PIN         9

#define LCD_ADDR 0x27

#define LCD_BACKLIGHT 0x08
#define LCD_ENABLE    0x04

#define CAP_MAX  4800.0f // mL
#define CAP_MIN  600.0f//ml

float setpoint = 2600.0f; // nível desejado

#define TEMPO_MAX_ENCHIMENTO_S 300000000000


absolute_time_t inicio_enchimento;

// ==========================
// LCD I2C (PCF8574)
// ==========================


void lcd_send_byte(uint8_t val) {
    i2c_write_blocking(I2C_PORT, LCD_ADDR, &val, 1, false);
}

void lcd_toggle_enable(uint8_t val) {
    sleep_us(1);
    lcd_send_byte(val | LCD_ENABLE);
    sleep_us(1);
    lcd_send_byte(val & ~LCD_ENABLE);
    sleep_us(100);
}

void lcd_send_nibble(uint8_t nibble, uint8_t mode) {
    uint8_t data = (nibble << 4) | mode | LCD_BACKLIGHT;
    lcd_send_byte(data);
    lcd_toggle_enable(data);
}

void lcd_send_cmd(uint8_t cmd) {
    lcd_send_nibble(cmd >> 4, 0);
    lcd_send_nibble(cmd & 0x0F, 0);
}

void lcd_send_char(char c) {
    lcd_send_nibble(c >> 4, 0x01);
    lcd_send_nibble(c & 0x0F, 0x01);
}

void lcd_init() {
    sleep_ms(50);

    lcd_send_nibble(0x03, 0);
    sleep_ms(5);

    lcd_send_nibble(0x03, 0);
    sleep_us(150);

    lcd_send_nibble(0x03, 0);
    lcd_send_nibble(0x02, 0);

    lcd_send_cmd(0x28);
    lcd_send_cmd(0x0C);
    lcd_send_cmd(0x06);
    lcd_send_cmd(0x01);

    sleep_ms(2);
}

void lcd_print(const char *str) {
    while (*str) {
        lcd_send_char(*str++);
    }
}

void lcd_clear()
{
    lcd_send_cmd(0x01);
    sleep_ms(2);
}

void lcd_set_cursor(uint8_t row, uint8_t col)
{
    uint8_t addr = col;

    if(row == 1)
        addr += 0x40;

    lcd_send_cmd(0x80 | addr);
}


// ==========================
// FUNÇÕES DE NÍVEL
// ==========================

float volume_inferior(float tensao)
{
    return exp((tensao + 8.7028f)/1.3781f); //valor solda //exp((tensao + 5.5865f)/1.0064f); //valor barra inox//exp((tensao + 4.3396f)/0.8455f); //valor 1 calibracao
}

float volume_superior(float tensao)
{
    return exp((tensao + 8.5945f)/1.37f); //valor solda //exp((tensao + 4.8861f)/0.9378f); //valor barra inox //exp((tensao + 4.3739f)/0.8518f);//valor 1 calibracao
}

// Histerese usando as duas curvas
float calcular_volume(float tensao)
{
    static float volume_estavel = 0;

    float vinf = volume_inferior(tensao);
    float vsup = volume_superior(tensao);

    float minimo = fmin(vinf,vsup);
    float maximo = fmax(vinf,vsup);

    if(volume_estavel >= minimo &&
       volume_estavel <= maximo)
    {
        return volume_estavel;
    }

    volume_estavel = (vinf + vsup)/2.0f;

    if(volume_estavel < 0)
        volume_estavel = 0;

    if(volume_estavel > CAP_MAX)
        volume_estavel = CAP_MAX;

    return volume_estavel;
}

// ==========================
// CONTROLE DA BOMBA
// ==========================

void bomba_parar()
{
    gpio_put(IN1,0);
    gpio_put(IN2,0);
}

void bomba_encher()
{
    gpio_put(IN1,1);
    gpio_put(IN2,0);
}

void bomba_esvaziar()
{
    gpio_put(IN1,0);
    gpio_put(IN2,1);
}
// ==========================
// CONTROLE COM HISTERESE
// ==========================

typedef enum
{
    PARADA,
    ENCHENDO,
    ESVAZIANDO,
    ALARME
} EstadoBomba;

// banda de histerese
#define BANDA_HISTERESE 200.0f

// tempo mínimo entre mudanças de estado
#define TEMPO_MINIMO_MS 2000

// ==========================
// MAIN
// ==========================

int main()
{
    stdio_init_all();
    sleep_ms(10000);

    printf(" INICIOU");
    // ADC
    adc_init();
    adc_gpio_init(ADC_PIN);
    adc_select_input(ADC_CHANNEL);

    // Ponte H
    gpio_init(IN1);
    gpio_set_dir(IN1, GPIO_OUT);

    gpio_init(IN2);
    gpio_set_dir(IN2, GPIO_OUT);


    bomba_parar();

    // I2C LCD
    i2c_init(I2C_PORT, 100000);

    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);

    gpio_pull_up(SDA_PIN);
    gpio_pull_up(SCL_PIN);

    lcd_init();

    char linha1[17];
    char linha2[17];

    char serial_buffer[20];
    int serial_index = 0;

    EstadoBomba estado = PARADA;

    absolute_time_t ultima_troca = get_absolute_time();

    while(1)
    {
        int c;
        // deixar o serial monitor o line ending em CR para poder alterar os valores
        while((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT)
        {
            //printf("Recebi: %c (%d)\n", c, c);
            if(c == '\n' || c == '\r')
            {
                serial_buffer[serial_index] = '\0';

                //printf("Texto recebido: [%s]\n", serial_buffer);

                if(strcmp(serial_buffer,"RESET") == 0)
                {
                    if(estado == ALARME)
                    {
                        estado = PARADA;

                        ultima_troca = get_absolute_time();

                        printf("ALARME RESETADO\n");
                    }

                    serial_index = 0;

                    continue;
                }

                float novo_setpoint = atof(serial_buffer);

                //printf("Convertido: %.2f\n", novo_setpoint);

                if(novo_setpoint >= 0 && novo_setpoint <= CAP_MAX)
                {
                    setpoint = novo_setpoint;

                    printf("Novo setpoint: %.0f mL\n", setpoint);
                }
                else
                {
                    printf("Valor invalido! Use 0 a %.0f mL\n", CAP_MAX);
                }

                serial_index = 0;
            }
            else
            {
                if(serial_index < sizeof(serial_buffer)-1)
                {
                    serial_buffer[serial_index++] = (char)c;
                    printf("Buffer: %s\n", serial_buffer);
                }
            }
           // printf("Char='%c' ASCII=%d\n", c, c);
        }
        uint16_t adc_raw = adc_read();

        float tensao = ((float)adc_raw * 3.3f) / 4095.0f;

        float volume = calcular_volume(tensao);
    
        // Incrementos de 200 mL
        int volume_display =
            ((int)(volume/200.0f))*200;
        if (volume_display < CAP_MIN)
        {
            volume_display=0;
        }
        // Controle do nível
        bool pode_mudar_estado =
        (
            absolute_time_diff_us(
                ultima_troca,
                get_absolute_time()
            ) > (TEMPO_MINIMO_MS * 1000)
        );

        //printf("Delta = %lld us\n",
          //  absolute_time_diff_us(
          //  ultima_troca,
           // get_absolute_time()));

           if(estado==ALARME)
            {
                bomba_parar();

                lcd_clear();

                lcd_set_cursor(0,0);
                lcd_print("ALARME");

                lcd_set_cursor(1,0);
                lcd_print("VAZAMENTO");

              

                sleep_ms(500);

                continue;
            }

        switch(estado)
        {
            case PARADA:

                if(pode_mudar_estado)
                {
                    if(volume < (setpoint - BANDA_HISTERESE))
                    {
                        estado = ENCHENDO;
                        ultima_troca = get_absolute_time();

                        inicio_enchimento = get_absolute_time();

                        printf("Entrou em ENCHENDO\n");
                    }
                    else if(volume > (setpoint + BANDA_HISTERESE))
                    {
                        estado = ESVAZIANDO;
                        ultima_troca = get_absolute_time();
                    }
                }

                break;

            case ENCHENDO:

                float tempo_enchendo =
                    absolute_time_diff_us(
                        inicio_enchimento,
                        get_absolute_time()
                    ) / 1000000.0f;

                if(tempo_enchendo > TEMPO_MAX_ENCHIMENTO_S)
                {
                    printf("\n");
                    printf("***************\n");
                    printf("ALARME VAZAMENTO\n");
                    printf("***************\n");

                    estado = ALARME;
                    break;
                }

                if(pode_mudar_estado)
                {
                    if(volume >= setpoint)
                    {
                        estado = PARADA;
                        ultima_troca = get_absolute_time();
                    }
                }

                break;

            case ESVAZIANDO:

                if(pode_mudar_estado)
                {
                    if(volume <= setpoint)
                    {
                        estado = PARADA;
                        ultima_troca = get_absolute_time();
                    }
                }

                break;

            case ALARME:
                break;
        }
        switch(estado)
        {
            case PARADA:
                bomba_parar();
                break;

            case ENCHENDO:
                bomba_encher();
                break;

            case ESVAZIANDO:
                bomba_esvaziar();
                break;

            case ALARME:
            bomba_parar();
            break;
        }

        sprintf(linha1,"Nivel:%4dmL",volume_display);
        sprintf(linha2,"SP:%4d",(int)setpoint);

        lcd_clear();

        lcd_set_cursor(0,0);
        lcd_print(linha1);

        lcd_set_cursor(1,0);
        lcd_print(linha2);

        printf(
        "Estado=%d  Vol=%.1f  SP=%.1f  Delta=%.2f S TENSAO=%.5f \n",
        estado,
        volume,
        setpoint,
        absolute_time_diff_us(
            ultima_troca,
            get_absolute_time()
        ) / 1000000.0f,
        tensao
    );

        sleep_ms(500);
    }

    return 0;
}