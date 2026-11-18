/* Issue #228: the self-test of check_jsonlock and check_rs485, a component that is not an owner.
 * It is never compiled. The comment and the strings must not count:
 * uart_good_in_comment(0); cJSON_good_in_comment(); */
int bad_web(const char *s)
{
    const char *good_text = "uart_good_in_string(0) cJSON_good_in_string";
    int r = cJSON_Parse(s) != 0;
    r += bad_bus_entry(1);
    r += gw_rs485_init(1);
    r += uart_write_bytes(1, good_text, 1);
    r += good_core_entry(1);
    return r;
}
