-- Добавление retry_parent_id для поддержки цепочек повторных попыток сессий

ALTER TABLE replay_sessions
ADD COLUMN retry_parent_id UUID REFERENCES replay_sessions(session_id) ON DELETE SET NULL;

-- Индекс для производительности при поиске retry-цепочек
CREATE INDEX IF NOT EXISTS replay_sessions_retry_parent_idx
ON replay_sessions(retry_parent_id);

-- Комментарий к колонке
COMMENT ON COLUMN replay_sessions.retry_parent_id IS 'Ссылка на родительскую сессию для формирования цепочки повторных попыток';