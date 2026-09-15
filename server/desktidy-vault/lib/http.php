<?php
declare(strict_types=1);

function vault_json_ok($data = null, int $code = 200): void
{
    http_response_code($code);
    header('Content-Type: application/json; charset=utf-8');
    header('Access-Control-Allow-Origin: *');
    header('Access-Control-Allow-Headers: Authorization, Content-Type');
    header('Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS');
    $payload = ['ok' => true];
    if ($data !== null) {
        $payload['data'] = $data;
    }
    echo json_encode($payload, JSON_UNESCAPED_UNICODE);
    exit;
}

function vault_json_err(string $error, string $message, int $code = 400): void
{
    http_response_code($code);
    header('Content-Type: application/json; charset=utf-8');
    header('Access-Control-Allow-Origin: *');
    header('Access-Control-Allow-Headers: Authorization, Content-Type');
    header('Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS');
    echo json_encode(
        ['ok' => false, 'error' => $error, 'message' => $message],
        JSON_UNESCAPED_UNICODE
    );
    exit;
}

function vault_read_json_body(): array
{
    $raw = file_get_contents('php://input');
    if ($raw === false || trim($raw) === '') {
        return [];
    }
    $data = json_decode($raw, true);
    return is_array($data) ? $data : [];
}

function vault_bearer_token(): ?string
{
    $hdr = $_SERVER['HTTP_AUTHORIZATION'] ?? $_SERVER['REDIRECT_HTTP_AUTHORIZATION'] ?? '';
    if (is_string($hdr) && preg_match('/^\s*Bearer\s+(\S+)\s*$/i', $hdr, $m)) {
        return $m[1];
    }
    $alt = $_SERVER['HTTP_X_VAULT_TOKEN'] ?? '';
    if (is_string($alt) && $alt !== '') {
        return $alt;
    }
    return null;
}
