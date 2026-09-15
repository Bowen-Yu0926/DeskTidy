<?php
declare(strict_types=1);

require_once __DIR__ . '/db.php';
require_once __DIR__ . '/http.php';

function vault_ensure_users_display_name(mysqli $db): void
{
    static $done = false;
    if ($done) {
        return;
    }
    $done = true;
    $res = $db->query("SHOW COLUMNS FROM vault_users LIKE 'display_name'");
    if ($res && $res->num_rows > 0) {
        return;
    }
    @$db->query(
        "ALTER TABLE vault_users
         ADD COLUMN display_name VARCHAR(40) NOT NULL DEFAULT '' AFTER login_type"
    );
}

function vault_normalize_login(string $login): array
{
    $login = trim($login);
    if ($login === '') {
        vault_json_err('validation', '请输入手机号或邮箱', 422);
    }
    if (preg_match('/^1\d{10}$/', $login)) {
        return [$login, 'phone'];
    }
    $lower = strtolower($login);
    if (filter_var($lower, FILTER_VALIDATE_EMAIL)) {
        return [$lower, 'email'];
    }
    vault_json_err('invalid_login', '请使用有效的手机号或邮箱', 422);
}

function vault_normalize_display_name(string $name): string
{
    $name = trim($name);
    // Collapse internal whitespace
    $name = preg_replace('/\s+/u', ' ', $name) ?? $name;
    $len = function_exists('mb_strlen') ? mb_strlen($name) : strlen($name);
    if ($len < 2) {
        vault_json_err('validation', '昵称至少 2 个字', 422);
    }
    if ($len > 20) {
        vault_json_err('validation', '昵称最多 20 个字', 422);
    }
    return $name;
}

function vault_issue_token(mysqli $db, int $userId): string
{
    $token = bin2hex(random_bytes(32));
    $hash = hash('sha256', $token);
    $days = (int)(vault_config()['token_ttl_days'] ?? 90);
    if ($days < 1) {
        $days = 90;
    }
    $expires = gmdate('Y-m-d H:i:s', time() + $days * 86400);
    $stmt = $db->prepare('INSERT INTO vault_tokens (user_id, token_hash, expires_at) VALUES (?, ?, ?)');
    $stmt->bind_param('iss', $userId, $hash, $expires);
    if (!$stmt->execute()) {
        vault_json_err('server', '无法签发登录态', 500);
    }
    return $token;
}

function vault_require_user(): array
{
    $token = vault_bearer_token();
    if ($token === null || $token === '') {
        vault_json_err('unauthorized', '请先登录', 401);
    }
    $hash = hash('sha256', $token);
    $db = vault_db();
    vault_ensure_users_display_name($db);
    $now = gmdate('Y-m-d H:i:s');
    $stmt = $db->prepare(
        'SELECT u.id, u.login, u.login_type, u.display_name, t.id AS token_id
         FROM vault_tokens t
         INNER JOIN vault_users u ON u.id = t.user_id
         WHERE t.token_hash = ? AND t.expires_at > ?
         LIMIT 1'
    );
    $stmt->bind_param('ss', $hash, $now);
    $stmt->execute();
    $row = $stmt->get_result()->fetch_assoc();
    if (!$row) {
        vault_json_err('unauthorized', '登录已失效，请重新登录', 401);
    }
    return [
        'id' => (int)$row['id'],
        'login' => (string)$row['login'],
        'login_type' => (string)$row['login_type'],
        'display_name' => (string)($row['display_name'] ?? ''),
        'token_id' => (int)$row['token_id'],
        'token_hash' => $hash,
    ];
}

function vault_auth_payload(int $userId, string $login, string $token, string $displayName): array
{
    return [
        'user_id' => $userId,
        'login' => $login,
        'display_name' => $displayName,
        'token' => $token,
    ];
}

function vault_handle_register(): void
{
    $body = vault_read_json_body();
    $password = (string)($body['password'] ?? '');
    if (strlen($password) < 6) {
        vault_json_err('validation', '密码至少 6 位', 422);
    }
    [$login, $type] = vault_normalize_login((string)($body['login'] ?? ''));
    $display = vault_normalize_display_name((string)($body['display_name'] ?? $body['nickname'] ?? ''));
    $db = vault_db();
    vault_ensure_users_display_name($db);
    $hash = password_hash($password, PASSWORD_DEFAULT);
    $stmt = $db->prepare(
        'INSERT INTO vault_users (login, login_type, display_name, password_hash) VALUES (?, ?, ?, ?)'
    );
    $stmt->bind_param('ssss', $login, $type, $display, $hash);
    if (!$stmt->execute()) {
        if ($db->errno === 1062) {
            vault_json_err('login_taken', '该账号已注册，请直接登录', 409);
        }
        vault_json_err('server', '注册失败', 500);
    }
    $userId = (int)$db->insert_id;
    $token = vault_issue_token($db, $userId);
    vault_json_ok(vault_auth_payload($userId, $login, $token, $display), 201);
}

function vault_handle_login(): void
{
    $body = vault_read_json_body();
    $password = (string)($body['password'] ?? '');
    [$login] = vault_normalize_login((string)($body['login'] ?? ''));
    $db = vault_db();
    vault_ensure_users_display_name($db);
    $stmt = $db->prepare(
        'SELECT id, password_hash, display_name FROM vault_users WHERE login = ? LIMIT 1'
    );
    $stmt->bind_param('s', $login);
    $stmt->execute();
    $row = $stmt->get_result()->fetch_assoc();
    if (!$row || !password_verify($password, (string)$row['password_hash'])) {
        vault_json_err('bad_credentials', '账号或密码错误', 401);
    }
    $userId = (int)$row['id'];
    $display = trim((string)($row['display_name'] ?? ''));
    $token = vault_issue_token($db, $userId);
    vault_json_ok(vault_auth_payload($userId, $login, $token, $display));
}

function vault_handle_logout(): void
{
    $user = vault_require_user();
    $db = vault_db();
    $tid = (int)$user['token_id'];
    $stmt = $db->prepare('DELETE FROM vault_tokens WHERE id = ?');
    $stmt->bind_param('i', $tid);
    $stmt->execute();
    vault_json_ok(['logged_out' => true]);
}
