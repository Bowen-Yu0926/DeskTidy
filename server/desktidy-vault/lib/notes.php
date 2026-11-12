<?php
declare(strict_types=1);

require_once __DIR__ . '/db.php';
require_once __DIR__ . '/crypto.php';
require_once __DIR__ . '/http.php';
require_once __DIR__ . '/auth_lib.php';

function vault_ensure_notes_table(mysqli $db): void
{
    static $done = false;
    if ($done) {
        return;
    }
    $done = true;
    $db->query(
        'CREATE TABLE IF NOT EXISTS vault_notes (
           id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
           user_id BIGINT UNSIGNED NOT NULL,
           title VARCHAR(200) NOT NULL,
           body_enc MEDIUMTEXT NOT NULL,
           enc_ver TINYINT NOT NULL DEFAULT 0,
           created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
           updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
           PRIMARY KEY (id),
           KEY idx_vault_notes_user (user_id),
           KEY idx_vault_notes_updated (user_id, updated_at),
           CONSTRAINT fk_vault_notes_user FOREIGN KEY (user_id)
             REFERENCES vault_users (id) ON DELETE CASCADE
         ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci'
    );
}

function vault_note_row_to_public(array $row, bool $include_body): array
{
    $out = [
        'id' => (int)$row['id'],
        'title' => (string)$row['title'],
        'enc_ver' => (int)($row['enc_ver'] ?? 0),
        'created_at' => (string)$row['created_at'],
        'updated_at' => (string)$row['updated_at'],
    ];
    if ($include_body) {
        $encVer = (int)($row['enc_ver'] ?? 0);
        if ($encVer === 1) {
            $out['body'] = (string)$row['body_enc'];
        } else {
            $out['body'] = vault_decrypt((string)$row['body_enc']);
        }
    }
    return $out;
}

function vault_fetch_note(mysqli $db, int $id, int $uid): ?array
{
    $stmt = $db->prepare(
        'SELECT id, title, body_enc, enc_ver, created_at, updated_at
         FROM vault_notes WHERE id = ? AND user_id = ? LIMIT 1'
    );
    $stmt->bind_param('ii', $id, $uid);
    $stmt->execute();
    $row = $stmt->get_result()->fetch_assoc();
    return $row ?: null;
}

function vault_parse_note_body(array $body, bool $require_title): array
{
    $title = trim((string)($body['title'] ?? ''));
    if ($require_title && $title === '') {
        vault_json_err('validation', '标题不能为空', 422);
    }
    if (function_exists('mb_strlen') ? mb_strlen($title) > 200 : strlen($title) > 200) {
        vault_json_err('validation', '标题过长', 422);
    }
    $text = (string)($body['body'] ?? '');
    if (strlen($text) > 2000000) {
        vault_json_err('validation', '正文过长', 422);
    }
    $encVer = (int)($body['enc_ver'] ?? 0);
    if ($encVer !== 1) {
        $encVer = 0;
    }
    return [
        'title' => $title === '' ? '未命名' : $title,
        'body' => $text,
        'enc_ver' => $encVer,
    ];
}

function vault_handle_notes_list(): void
{
    $user = vault_require_user();
    $db = vault_db();
    vault_ensure_notes_table($db);
    $uid = (int)$user['id'];
    $stmt = $db->prepare(
        'SELECT id, title, body_enc, enc_ver, created_at, updated_at
         FROM vault_notes WHERE user_id = ? ORDER BY updated_at DESC, id DESC'
    );
    $stmt->bind_param('i', $uid);
    $stmt->execute();
    $res = $stmt->get_result();
    $items = [];
    while ($row = $res->fetch_assoc()) {
        $items[] = vault_note_row_to_public($row, false);
    }
    vault_json_ok(['items' => $items]);
}

function vault_handle_notes_get(int $id): void
{
    $user = vault_require_user();
    $db = vault_db();
    vault_ensure_notes_table($db);
    $row = vault_fetch_note($db, $id, (int)$user['id']);
    if (!$row) {
        vault_json_err('not_found', '笔记不存在', 404);
    }
    vault_json_ok(vault_note_row_to_public($row, true));
}

function vault_handle_notes_create(): void
{
    $user = vault_require_user();
    $fields = vault_parse_note_body(vault_read_json_body(), true);
    $db = vault_db();
    vault_ensure_notes_table($db);
    $uid = (int)$user['id'];
    if ($fields['enc_ver'] === 1) {
        $bodyEnc = $fields['body'];
    } else {
        $bodyEnc = vault_encrypt($fields['body']);
    }
    $stmt = $db->prepare(
        'INSERT INTO vault_notes (user_id, title, body_enc, enc_ver)
         VALUES (?, ?, ?, ?)'
    );
    $stmt->bind_param('issi', $uid, $fields['title'], $bodyEnc, $fields['enc_ver']);
    if (!$stmt->execute()) {
        vault_json_err('server', '保存失败', 500);
    }
    $id = (int)$db->insert_id;
    $row = vault_fetch_note($db, $id, $uid);
    vault_json_ok(vault_note_row_to_public($row, true), 201);
}

function vault_handle_notes_update(int $id): void
{
    $user = vault_require_user();
    $fields = vault_parse_note_body(vault_read_json_body(), false);
    $db = vault_db();
    vault_ensure_notes_table($db);
    $uid = (int)$user['id'];
    $existing = vault_fetch_note($db, $id, $uid);
    if (!$existing) {
        vault_json_err('not_found', '笔记不存在', 404);
    }
    $title = $fields['title'];
    if (trim((string)($fields['title'] ?? '')) === '' && isset($existing['title'])) {
        $title = (string)$existing['title'];
    }
    if ($fields['enc_ver'] === 1) {
        $bodyEnc = $fields['body'];
    } else {
        $bodyEnc = vault_encrypt($fields['body']);
    }
    $stmt = $db->prepare(
        'UPDATE vault_notes SET title = ?, body_enc = ?, enc_ver = ?
         WHERE id = ? AND user_id = ?'
    );
    $stmt->bind_param('ssiii', $title, $bodyEnc, $fields['enc_ver'], $id, $uid);
    if (!$stmt->execute()) {
        vault_json_err('server', '保存失败', 500);
    }
    $row = vault_fetch_note($db, $id, $uid);
    vault_json_ok(vault_note_row_to_public($row, true));
}

function vault_handle_notes_delete(int $id): void
{
    $user = vault_require_user();
    $db = vault_db();
    vault_ensure_notes_table($db);
    $uid = (int)$user['id'];
    $stmt = $db->prepare('DELETE FROM vault_notes WHERE id = ? AND user_id = ?');
    $stmt->bind_param('ii', $id, $uid);
    if (!$stmt->execute()) {
        vault_json_err('server', '删除失败', 500);
    }
    if ($stmt->affected_rows < 1) {
        vault_json_err('not_found', '笔记不存在', 404);
    }
    vault_json_ok(['deleted' => true]);
}
