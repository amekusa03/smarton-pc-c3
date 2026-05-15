#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void lcd_init(void);
void lcd_notify_wifi_connecting(void);
void lcd_notify_wifi_connected(const char *ip);
void lcd_notify_commissioning(void);
void lcd_notify_pc_on(void);
void lcd_notify_pc_off(void);

#ifdef __cplusplus
}
#endif
