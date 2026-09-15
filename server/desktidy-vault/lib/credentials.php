<?php
declare(strict_types=1);

require_once __DIR__ . '/db.php';
require_once __DIR__ . '/crypto.php';
require_once __DIR__ . '/http.php';
require_once __DIR__ . '/auth_lib.php';

/** True for true / 1 / "1" / "true" (case-insensitive); false otherwise (incl. JSON false). */
function vault_boolish(mixed $value): bool
{
    if ($value === true || $value === 1 || $value === 1.0) {
        return true;
    }
    if ($value === false || $value === 0 || $value === 0.0 || $value === null) {
        return false;
    }
    if (is_string($value)) {
        $v = strtolower(trim($value));
        return in_array($v, ['1', 'true', 'yes', 'on'], true);
    }
    return false;
}

function vault_ensure_credentials_columns(mysqli $db): void
{
    static $done = false;
    if ($done) {
        return;
    }
    $done = true;

    $res = $db->query("SHOW COLUMNS FROM vault_credentials LIKE 'pinned'");
    if (!($res && $res->num_rows > 0)) {
        @$db->query(
            'ALTER TABLE vault_credentials
             ADD COLUMN pinned TINYINT(1) NOT NULL DEFAULT 0 AFTER note_enc,
             ADD KEY idx_vault_credentials_pinned (user_id, pinned)'
        );
    }

    $res = $db->query("SHOW COLUMNS FROM vault_credentials LIKE 'sort_order'");
    if (!($res && $res->num_rows > 0)) {
        @$db->query(
            'ALTER TABLE vault_credentials
             ADD COLUMN sort_order INT NOT NULL DEFAULT 0 AFTER pinned,
             ADD KEY idx_vault_credentials_sort (user_id, pinned, sort_order)'
        );
        // Stable initial order for existing rows.
        @$db->query('UPDATE vault_credentials SET sort_order = id WHERE sort_order = 0');
    }
}

/** @deprecated use vault_ensure_credentials_columns */
function vault_ensure_credentials_pinned_column(mysqli $db): void
{
    vault_ensure_credentials_columns($db);
}

function vault_cred_select_sql(): string
{
    return 'SELECT id, title, username_enc, password_enc, url_enc, note_enc, pinned, sort_order, created_at, updated_at
            FROM vault_credentials';
}

function vault_cred_row_to_public(array $row): array
{
    return [
        'id' => (int)$row['id'],
        'title' => (string)$row['title'],
        'username' => vault_decrypt((string)$row['username_enc']),
        'password' => vault_decrypt((string)$row['password_enc']),
        'url' => vault_decrypt((string)$row['url_enc']),
        'note' => vault_decrypt((string)$row['note_enc']),
        'pinned' => !empty($row['pinned']),
        'sort_order' => (int)($row['sort_order'] ?? 0),
        'created_at' => (string)$row['created_at'],
        'updated_at' => (string)$row['updated_at'],
    ];
}

function vault_fetch_credential(mysqli $db, int $id, int $uid): ?array
{
    $sql = vault_cred_select_sql() . ' WHERE id = ? AND user_id = ? LIMIT 1';
    $stmt = $db->prepare($sql);
    $stmt->bind_param('ii', $id, $uid);
    $stmt->execute();
    $row = $stmt->get_result()->fetch_assoc();
    return $row ?: null;
}

function vault_next_sort_order(mysqli $db, int $uid): int
{
    $stmt = $db->prepare(
        'SELECT COALESCE(MAX(sort_order), 0) + 1 AS n FROM vault_credentials WHERE user_id = ?'
    );
    $stmt->bind_param('i', $uid);
    $stmt->execute();
    $row = $stmt->get_result()->fetch_assoc();
    return (int)($row['n'] ?? 1);
}

function vault_handle_credentials_list(): void
{
    $user = vault_require_user();
    $db = vault_db();
    vault_ensure_credentials_columns($db);
    $uid = (int)$user['id'];
    $stmt = $db->prepare(
        vault_cred_select_sql()
        . ' WHERE user_id = ? ORDER BY pinned DESC, sort_order ASC, id ASC'
    );
    $stmt->bind_param('i', $uid);
    $stmt->execute();
    $res = $stmt->get_result();
    $items = [];
    while ($row = $res->fetch_assoc()) {
        $items[] = vault_cred_row_to_public($row);
    }
    vault_json_ok(['items' => $items]);
}

function vault_parse_credential_body(array $body, bool $requireTitle): array
{
    $title = trim((string)($body['title'] ?? ''));
    if ($requireTitle && $title === '') {
        vault_json_err('validation', '名称不能为空', 422);
    }
    if (function_exists('mb_strlen') ? mb_strlen($title) > 200 : strlen($title) > 200) {
        vault_json_err('validation', '名称过长', 422);
    }
    $pinned = null;
    if (array_key_exists('pinned', $body)) {
        $pinned = vault_boolish($body['pinned']) ? 1 : 0;
    }
    return [
        'title' => $title,
        'username' => trim((string)($body['username'] ?? '')),
        'password' => trim((string)($body['password'] ?? '')),
        'url' => trim((string)($body['url'] ?? '')),
        'note' => trim((string)($body['note'] ?? '')),
        'pinned' => $pinned,
    ];
}

function vault_handle_credentials_create(): void
{
    $user = vault_require_user();
    $fields = vault_parse_credential_body(vault_read_json_body(), true);
    $db = vault_db();
    vault_ensure_credentials_columns($db);
    $uid = (int)$user['id'];
    $uEnc = vault_encrypt($fields['username']);
    $pEnc = vault_encrypt($fields['password']);
    $urlEnc = vault_encrypt($fields['url']);
    $nEnc = vault_encrypt($fields['note']);
    $pinned = $fields['pinned'] === null ? 0 : (int)$fields['pinned'];
    $sort = vault_next_sort_order($db, $uid);
    $stmt = $db->prepare(
        'INSERT INTO vault_credentials
         (user_id, title, username_enc, password_enc, url_enc, note_enc, pinned, sort_order)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?)'
    );
    $stmt->bind_param(
        'isssssii',
        $uid,
        $fields['title'],
        $uEnc,
        $pEnc,
        $urlEnc,
        $nEnc,
        $pinned,
        $sort
    );
    if (!$stmt->execute()) {
        vault_json_err('server', '保存失败', 500);
    }
    $id = (int)$db->insert_id;
    $row = vault_fetch_credential($db, $id, $uid);
    vault_json_ok(vault_cred_row_to_public($row), 201);
}

function vault_handle_credentials_update(int $id): void
{
    $user = vault_require_user();
    $body = vault_read_json_body();
    $db = vault_db();
    vault_ensure_credentials_columns($db);
    $uid = (int)$user['id'];
    $existing = vault_fetch_credential($db, $id, $uid);
    if (!$existing) {
        vault_json_err('not_found', '条目不存在', 404);
    }

    // Pin-only update: { "pinned": true/false }
    if (array_key_exists('pinned', $body) && !array_key_exists('title', $body)) {
        $pinned = vault_boolish($body['pinned']) ? 1 : 0;
        $stmt = $db->prepare(
            'UPDATE vault_credentials SET pinned = ? WHERE id = ? AND user_id = ?'
        );
        $stmt->bind_param('iii', $pinned, $id, $uid);
        if (!$stmt->execute()) {
            vault_json_err('server', '更新失败', 500);
        }
        $row = vault_fetch_credential($db, $id, $uid);
        vault_json_ok(vault_cred_row_to_public($row));
    }

    $fields = vault_parse_credential_body($body, true);
    $uEnc = vault_encrypt($fields['username']);
    $pEnc = vault_encrypt($fields['password']);
    $urlEnc = vault_encrypt($fields['url']);
    $nEnc = vault_encrypt($fields['note']);
    $pinned = $fields['pinned'] === null
        ? (int)!empty($existing['pinned'])
        : (int)$fields['pinned'];
    $stmt = $db->prepare(
        'UPDATE vault_credentials
         SET title = ?, username_enc = ?, password_enc = ?, url_enc = ?, note_enc = ?, pinned = ?
         WHERE id = ? AND user_id = ?'
    );
    $stmt->bind_param(
        'sssssiii',
        $fields['title'],
        $uEnc,
        $pEnc,
        $urlEnc,
        $nEnc,
        $pinned,
        $id,
        $uid
    );
    if (!$stmt->execute()) {
        vault_json_err('server', '更新失败', 500);
    }
    $row = vault_fetch_credential($db, $id, $uid);
    if (!$row) {
        vault_json_err('not_found', '条目不存在', 404);
    }
    vault_json_ok(vault_cred_row_to_public($row));
}

/**
 * Body: { "ids": [3, 1, 2] } — full ordered id list for the user (pinned + unpinned).
 * Assigns sort_order = index; does not change pinned flags.
 */
function vault_handle_credentials_reorder(): void
{
    $user = vault_require_user();
    $body = vault_read_json_body();
    $idsRaw = $body['ids'] ?? null;
    if (!is_array($idsRaw) || $idsRaw === []) {
        vault_json_err('validation', 'ids 不能为空', 422);
    }
    $ids = [];
    foreach ($idsRaw as $v) {
        $id = (int)$v;
        if ($id < 1) {
            vault_json_err('validation', 'ids 无效', 422);
        }
        $ids[] = $id;
    }
    if (count($ids) !== count(array_unique($ids))) {
        vault_json_err('validation', 'ids 不能重复', 422);
    }

    $db = vault_db();
    vault_ensure_credentials_columns($db);
    $uid = (int)$user['id'];

    $stmt = $db->prepare('SELECT id FROM vault_credentials WHERE user_id = ?');
    $stmt->bind_param('i', $uid);
    $stmt->execute();
    $res = $stmt->get_result();
    $owned = [];
    while ($row = $res->fetch_assoc()) {
        $owned[(int)$row['id']] = true;
    }
    if (count($owned) !== count($ids)) {
        vault_json_err('validation', '须提交当前账号下的全部条目 id', 422);
    }
    foreach ($ids as $id) {
        if (!isset($owned[$id])) {
            vault_json_err('validation', '包含无权条目', 403);
        }
    }

    $upd = $db->prepare(
        'UPDATE vault_credentials SET sort_order = ? WHERE id = ? AND user_id = ?'
    );
    foreach ($ids as $i => $id) {
        $sort = (int)$i;
        $upd->bind_param('iii', $sort, $id, $uid);
        if (!$upd->execute()) {
            vault_json_err('server', '排序保存失败', 500);
        }
    }
    vault_json_ok(['reordered' => true, 'count' => count($ids)]);
}

function vault_handle_credentials_delete(int $id): void
{
    $user = vault_require_user();
    $db = vault_db();
    $uid = (int)$user['id'];
    $stmt = $db->prepare('DELETE FROM vault_credentials WHERE id = ? AND user_id = ?');
    $stmt->bind_param('ii', $id, $uid);
    $stmt->execute();
    if ($stmt->affected_rows < 1) {
        vault_json_err('not_found', '条目不存在', 404);
    }
    vault_json_ok(['deleted' => true]);
}
