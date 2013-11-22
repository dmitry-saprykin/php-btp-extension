<?php

if (!extension_loaded("btp")) {

    /*
     * INI_ENTRIES
     *
     * btp.cli_enable
     *
     * Разрешает или запрещает работу со счетчиками в cli режиме.
     * Default - 1
     *
     *
     * btp.fpm_enable
     *
     * Разрешает или запрещает работу со счетчиками в не cli режиме.
     * Default - 1
     *
     *
     * btp.autoflush_time
     *
     * Автоматическая отправка счетчиков через указанное количество секунд.
     * 0 - не отправлять автоматически.
     * Default - 60.
     *
     *
     * btp.autoflush_count
     *
     * Автоматическая отправка счетчиков пачками размером count.
     * 0 - не отправлять автоматически.
     * Default - 0.
     */

    /**
     * Добавить коннект к серверу btp.
     *
     * @param int $id
     * @param string $host
     * @param string $port
     *
     * @return bool
     */
    function btp_config_server_set($id, $host, $port) {
    }

    /**
     * Установить имя скрипта.
     *
     * @param string $script
     *
     * @return bool
     */
    function btp_script_name_set($script) {
    }

    /**
     * Старт таймера.
     *
     * @param string $service
     * @param string $server
     * @param string $operation
     * @param int $serverid
     *
     * @return resource
     */
    function btp_timer_start($service, $server, $operation, $serverid = 0) {
    }

    /**
     * Стop таймера.
     *
     * @param resource $timer
     *
     * @return bool
     */
    function btp_timer_stop($timer) {
    }

    /**
     * Создать остановленный счетчик c явно указанным временем.
     *
     * @param string $service
     * @param string $server
     * @param string $operation
     * @param int $time
     * @param int $serverid
     *
     * @return bool
     */
    function btp_timer_count($service, $server, $operation, $time = 0, $serverid = 0) {
    }

    /**
     * Создать остановленный счетчик c явно указанным временем и именем скрипта.
     *
     * @param string $service
     * @param string $server
     * @param string $operation
     * @param string $script
     * @param int $time
     * @param int $serverid
     *
     * @return bool
     */
    function btp_timer_count_script($service, $server, $operation, $script, $time = 0, $serverid = 0) {
    }

    /**
     * Установить операцию для неостановленного таймера.
     *
     * @param resource $timer
     * @param string $operation
     *
     * @return bool
     */
    function btp_timer_set_operation($timer, $operation) {
    }

    /**
     * Дамп данных расширения.
     *
     * @return array
     */
    function btp_dump() {
    }

    /**
     * Дамп данных расширения.
     *
     * @param resource $timer
     *
     * @return array
     */
    function btp_dump_timer($timer) {
    }

    /**
     * Отправить счетчики.
     *
     * @param bool $stopped - только остановленные или предварительно все остановить.
     *
     * @return bool
     */
    function btp_flush($stopped = true) {
    }
}