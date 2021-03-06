void usb_init(void);
bool usb_vcp_is_enabled(void);
int usb_vcp_rx_any(void);
char usb_vcp_rx_get(void);
void usb_vcp_send_str(const char* str);
void usb_vcp_send_strn(const char* str, int len);
void usb_vcp_send_strn_cooked(const char *str, int len);
void usb_hid_send_report(uint8_t *buf); // 4 bytes for mouse: ?, x, y, ?

void pyb_usbh_init(void);
void pyb_usbh_process(void);
