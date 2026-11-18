/* Issue #228: the self-test of check_rs485. bad_bus_entry reaches the Modbus master, so only the
 * owners may call it (components/gw_web/bad_web.c calls it). bad_uart_core calls the UART in the
 * core component. good_core_entry does not reach the master. */
int mb_master_poke(int unit);
int uart_flush(int port);

int bad_bus_entry(int unit)
{
    return mb_master_poke(unit);
}

int bad_uart_core(void)
{
    return uart_flush(1);
}

int good_core_entry(int unit)
{
    return unit + 1;
}
