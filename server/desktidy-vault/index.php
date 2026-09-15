<?php
declare(strict_types=1);

require_once __DIR__ . '/lib/http.php';
require_once __DIR__ . '/lib/auth_lib.php';
require_once __DIR__ . '/lib/credentials.php';

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    vault_json_ok(['pong' => true]);
}

/**
 * Resolve route path:
 * - Pretty: /desktidy-vault/auth/login
 * - Query:  /desktidy-vault/index.php?r=auth/login
 * - PATH_INFO: /desktidy-vault/index.php/auth/login
 */
function vault_route_path(): string
{
    if (!empty($_GET['r']) && is_string($_GET['r'])) {
        return trim($_GET['r'], '/');
    }
    $pathInfo = $_SERVER['PATH_INFO'] ?? '';
    if (is_string($pathInfo) && $pathInfo !== '') {
        return trim($pathInfo, '/');
    }
    $uri = parse_url($_SERVER['REQUEST_URI'] ?? '', PHP_URL_PATH);
    $uri = is_string($uri) ? $uri : '';
    $script = $_SERVER['SCRIPT_NAME'] ?? '';
    $base = rtrim(str_replace('\\', '/', dirname($script)), '/');
    if ($base !== '' && strpos($uri, $base) === 0) {
        $uri = substr($uri, strlen($base));
    }
    $uri = trim($uri, '/');
    if ($uri === 'index.php' || strpos($uri, 'index.php/') === 0) {
        $uri = trim(substr($uri, strlen('index.php')), '/');
    }
    return $uri;
}

try {
    $method = strtoupper($_SERVER['REQUEST_METHOD'] ?? 'GET');
    $path = vault_route_path();

    if ($path === '' || $path === 'health') {
        vault_json_ok(['service' => 'desktidy-vault', 'version' => 1]);
    }

    if ($path === 'auth/register' && $method === 'POST') {
        vault_handle_register();
    }
    if ($path === 'auth/login' && $method === 'POST') {
        vault_handle_login();
    }
    if ($path === 'auth/logout' && $method === 'POST') {
        vault_handle_logout();
    }

    if ($path === 'credentials' && $method === 'GET') {
        vault_handle_credentials_list();
    }
    if ($path === 'credentials' && $method === 'POST') {
        vault_handle_credentials_create();
    }
    if ($path === 'credentials/reorder' && ($method === 'PUT' || $method === 'POST')) {
        vault_handle_credentials_reorder();
    }

    if (preg_match('#^credentials/(\d+)$#', $path, $m)) {
        $id = (int)$m[1];
        if ($method === 'PUT' || $method === 'PATCH') {
            vault_handle_credentials_update($id);
        }
        if ($method === 'DELETE') {
            vault_handle_credentials_delete($id);
        }
    }

    vault_json_err('not_found', '未知接口: ' . $method . ' ' . $path, 404);
} catch (Throwable $e) {
    vault_json_err('server', '服务器错误: ' . $e->getMessage(), 500);
}
