<?php
declare(strict_types=1);

require_once __DIR__ . '/db.php';

function vault_data_key(): string
{
    $hex = (string)(vault_config()['vault_data_key_hex'] ?? '');
    $key = @hex2bin($hex);
    if ($key === false || strlen($key) !== 32) {
        throw new RuntimeException('vault_data_key_hex must be 64 hex chars (32 bytes)');
    }
    return $key;
}

/** Encrypt UTF-8 plaintext; returns base64(iv|tag|ciphertext). */
function vault_encrypt(string $plain): string
{
    $key = vault_data_key();
    $iv = random_bytes(12);
    $tag = '';
    $cipher = openssl_encrypt($plain, 'aes-256-gcm', $key, OPENSSL_RAW_DATA, $iv, $tag, '', 16);
    if ($cipher === false || $tag === '') {
        throw new RuntimeException('encrypt failed');
    }
    return base64_encode($iv . $tag . $cipher);
}

function vault_decrypt(string $encoded): string
{
    $raw = base64_decode($encoded, true);
    if ($raw === false || strlen($raw) < 28) {
        return '';
    }
    $iv = substr($raw, 0, 12);
    $tag = substr($raw, 12, 16);
    $cipher = substr($raw, 28);
    $key = vault_data_key();
    $plain = openssl_decrypt($cipher, 'aes-256-gcm', $key, OPENSSL_RAW_DATA, $iv, $tag);
    return $plain === false ? '' : $plain;
}
