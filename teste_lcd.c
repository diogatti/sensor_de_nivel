#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#define I2C_PORT i2c0
#define SDA_PIN 8
#define SCL_PIN 9

#define LCD_ADDR 0x27

#define LCD_BACKLIGHT 0x08
#define LCD_ENABLE    0x04

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

int main() {
    stdio_init_all();

    i2c_init(I2C_PORT, 100000);

    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);

    gpio_pull_up(SDA_PIN);
    gpio_pull_up(SCL_PIN);

    sleep_ms(1000);

    lcd_init();
    lcd_print("Hello World");

    while (true) {
        sleep_ms(1000);
    }
}