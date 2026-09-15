<?php
/**
 * Copy to config.local.php and fill secrets. Do NOT commit config.local.php.
 */
return [
    'db_host' => 'sql110.byethost10.com',
    'db_name' => 'b10_42906238_vault', // change to your created DB name
    'db_user' => 'b10_42906238',
    'db_pass' => 'CHANGE_ME',
    'db_charset' => 'utf8mb4',
    // 64 hex chars (32 bytes) — generate: bin2hex(random_bytes(32))
    'vault_data_key_hex' => '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',
    'token_ttl_days' => 90,
];
