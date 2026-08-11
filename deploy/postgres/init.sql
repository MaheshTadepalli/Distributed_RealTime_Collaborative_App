CREATE TABLE IF NOT EXISTS documents (
  id TEXT PRIMARY KEY,
  content TEXT NOT NULL,
  revision BIGINT NOT NULL DEFAULT 0,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS operations (
  id BIGSERIAL PRIMARY KEY,
  document_id TEXT NOT NULL REFERENCES documents(id),
  revision BIGINT NOT NULL,
  op_id TEXT NOT NULL,
  client_id TEXT NOT NULL,
  op_type TEXT NOT NULL,
  position BIGINT NOT NULL,
  text TEXT NOT NULL DEFAULT '',
  op_count BIGINT NOT NULL DEFAULT 0,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  UNIQUE(document_id, revision)
);

CREATE INDEX IF NOT EXISTS operations_doc_rev_idx ON operations(document_id, revision);
