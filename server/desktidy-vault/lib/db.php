<?php
declare(strict_types=1);

function vault_config(): array
{
    static $cfg = null;
    if ($cfg !== null) {
        return $cfg;
    }
    $local = __DIR__ . '/../config.local.php';
    $example = __DIR__ . '/../config.example.php';
    if (is_file($local)) {
        $cfg = require $local;
    } elseif (is_file($example)) {
        $cfg = require $example;
    } else {
        throw new RuntimeException('Missing config.local.php');
    }
    if (!is_array($cfg)) {
        throw new RuntimeException('Invalid config');
    }
    return $cfg;
}

function vault_db(): mysqli
{
    static $db = null;
    if ($db instanceof mysqli) {
        return $db;
    }
    $c = vault_config();
    $db = new mysqli(
        (string)$c['db_host'],
        (string)$c['db_user'],
        (string)$c['db_pass'],
        (string)$c['db_name']
    );
    if ($db->connect_errno) {
        throw new RuntimeException('DB connect failed: ' . $db->connect_error);
    }
    $charset = (string)($c['db_charset'] ?? 'utf8mb4');
    $db->set_charset($charset);
    return $db;
}
