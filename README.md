php-btp-extension
=================

Php extension to publish execution statistics to Btp statistics daemon.<br>
Btp daemon code and docs can be found here - [Btp statistics][1]

Extension supports working with many btp servers.
Counters are sent by UDP requests on script finish.
Also counters can be sent during script execution to save memory
(see {btp.autoflush_time} and {btp.autoflush_count} settings).

##Ini settings

<b>btp.cli_enable</b><br>
Enable extension in cli mode.<br>
Default - 1

<b>btp.fpm_enable</b><br>
Enable extension in fpm mode.<br>
Default  - 1

<b>btp.autoflush_time</b><br>", "60", PHP_INI_ALL, OnIniUpdate)
Send stopped timers and counters eash {btp.autoflush_time} seconds. 0 means disabled<br>
Default - 60.

<b>btp.autoflush_count</b><br>
Send stopped timers and counters after {btp.autoflush_count} was added.<br>
Default - 0 (disabled)

##Functions

<b>bool btp_config_server_set( int $id, string $host, string $port )</b>

Adds new server connect with $id.


<b>bool btp_script_name_set( string $script )</b>

Sets default counter scriptname.

<b>resource btp_timer_start( string $service, string $server, string $operation, int $serverid = 0 )</b>

Starts timer.

<b>bool btp_timer_stop( $timer )</b>

Stops timer.

<b>bool btp_timer_count( string $service, string $server, string $operation, int $time = 0, int $serverid = 0)</b>

Creates counter. $time is microseconds.

<b>bool btp_timer_count_script( string $service, string $server, string $operation, string $script, int $time = 0, int $serverid = 0)</b>

Creates counter with custom scriptname. $time is microseconds.

<b>bool btp_timer_set_operation( resource $timer, string $operation)</b>

Changes operation for non stopped timer. If timer stopped returns false and produces Warning.

<b>array btp_dump()</b>

Returns current timers and counters list. List contains just items which has not been sent yet.

<b>bool btp_flush($stopped = true)</b>

Sends counters and stopped timers. If $stopped == false stops all timers before sending.

[1]: https://github.com/mambaru/btp-daemon
