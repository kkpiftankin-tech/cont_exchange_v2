BEGIN;

-- F15-RBAC-1: Role-Based Access Control Schema
-- Manages roles, permissions, and user role assignments for replay API.
-- System roles (admin, analyst, viewer) are pre-populated.

-- Core roles table
CREATE TABLE IF NOT EXISTS roles (
  role_id UUID PRIMARY KEY,
  role_name VARCHAR(255) NOT NULL UNIQUE,
  description TEXT,
  is_system BOOLEAN NOT NULL DEFAULT FALSE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  created_by TEXT NOT NULL,
  CHECK (role_name ~ '^[a-z_][a-z0-9_]*$')
);

CREATE INDEX IF NOT EXISTS roles_name_idx ON roles(role_name);

-- Permissions table (granular permissions)
CREATE TABLE IF NOT EXISTS permissions (
  permission_id UUID PRIMARY KEY,
  permission_name VARCHAR(255) NOT NULL UNIQUE,
  resource_type VARCHAR(64) NOT NULL,
  action VARCHAR(64) NOT NULL,
  description TEXT,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  UNIQUE(resource_type, action)
);

CREATE INDEX IF NOT EXISTS permissions_resource_action_idx
  ON permissions(resource_type, action);

-- Junction table: roles -> permissions (many-to-many)
CREATE TABLE IF NOT EXISTS role_permissions (
  role_permission_id UUID PRIMARY KEY,
  role_id UUID NOT NULL REFERENCES roles(role_id) ON DELETE CASCADE,
  permission_id UUID NOT NULL REFERENCES permissions(permission_id) ON DELETE CASCADE,
  granted_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  granted_by TEXT NOT NULL,
  UNIQUE(role_id, permission_id)
);

CREATE INDEX IF NOT EXISTS role_permissions_role_id_idx
  ON role_permissions(role_id);
CREATE INDEX IF NOT EXISTS role_permissions_permission_id_idx
  ON role_permissions(permission_id);

-- User role assignments
CREATE TABLE IF NOT EXISTS users_roles (
  user_role_id UUID PRIMARY KEY,
  user_id TEXT NOT NULL,
  role_id UUID NOT NULL REFERENCES roles(role_id) ON DELETE CASCADE,
  assigned_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  assigned_by TEXT NOT NULL,
  expires_at TIMESTAMPTZ,
  UNIQUE(user_id, role_id)
);

CREATE INDEX IF NOT EXISTS users_roles_user_id_idx ON users_roles(user_id);
CREATE INDEX IF NOT EXISTS users_roles_role_id_idx ON users_roles(role_id);
CREATE INDEX IF NOT EXISTS users_roles_expires_at_idx ON users_roles(expires_at)
  WHERE expires_at IS NOT NULL;

-- F15-RBAC-2: Comprehensive audit trail for all replay operations
CREATE TABLE IF NOT EXISTS audit_log (
  audit_id UUID PRIMARY KEY,
  session_id UUID REFERENCES replay_sessions(session_id) ON DELETE SET NULL,
  user_id TEXT NOT NULL,
  actor_role_ids UUID[] NOT NULL,
  resource_type VARCHAR(64) NOT NULL,
  resource_id TEXT NOT NULL,
  action VARCHAR(64) NOT NULL,
  status VARCHAR(32) NOT NULL CHECK (status IN ('success', 'failure', 'denied')),
  old_values JSONB,
  new_values JSONB,
  ip_address INET,
  user_agent TEXT,
  correlation_id TEXT,
  error_details TEXT,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  CHECK (resource_id != '')
);

CREATE INDEX IF NOT EXISTS audit_log_user_id_created_idx
  ON audit_log(user_id, created_at DESC);
CREATE INDEX IF NOT EXISTS audit_log_resource_idx
  ON audit_log(resource_type, resource_id);
CREATE INDEX IF NOT EXISTS audit_log_status_idx ON audit_log(status);
CREATE INDEX IF NOT EXISTS audit_log_created_range_idx ON audit_log(created_at DESC);
CREATE INDEX IF NOT EXISTS audit_log_session_id_idx ON audit_log(session_id);
CREATE INDEX IF NOT EXISTS audit_log_correlation_id_idx ON audit_log(correlation_id);

-- Insert default system roles
INSERT INTO roles (role_id, role_name, description, is_system, created_by)
VALUES
  ('11111111-1111-1111-1111-111111111111'::UUID, 'admin',
   'Full access to all replay operations and audit logs', true, 'system'),
  ('22222222-2222-2222-2222-222222222222'::UUID, 'analyst',
   'Can create and execute replay sessions, read own sessions', true, 'system'),
  ('33333333-3333-3333-3333-333333333333'::UUID, 'viewer',
   'Read-only access to own replay sessions', true, 'system')
ON CONFLICT (role_name) DO NOTHING;

-- Insert permissions
INSERT INTO permissions (permission_id, permission_name, resource_type, action, description)
VALUES
  ('aaaa0001-aaaa-aaaa-aaaa-aaaaaaaaaaaa'::UUID, 'replay:create', 'replay_session', 'create',
   'Create new replay session'),
  ('aaaa0002-aaaa-aaaa-aaaa-aaaaaaaaaaaa'::UUID, 'replay:read', 'replay_session', 'read',
   'Read replay sessions'),
  ('aaaa0003-aaaa-aaaa-aaaa-aaaaaaaaaaaa'::UUID, 'replay:execute', 'replay_session', 'execute',
   'Execute replay session'),
  ('aaaa0004-aaaa-aaaa-aaaa-aaaaaaaaaaaa'::UUID, 'replay:cancel', 'replay_session', 'cancel',
   'Cancel replay session'),
  ('aaaa0005-aaaa-aaaa-aaaa-aaaaaaaaaaaa'::UUID, 'audit:read', 'audit_log', 'read',
   'Read audit logs')
ON CONFLICT (permission_name) DO NOTHING;

-- Assign permissions to admin role (all permissions)
INSERT INTO role_permissions (role_permission_id, role_id, permission_id, granted_by)
SELECT
  gen_random_uuid(),
  '11111111-1111-1111-1111-111111111111'::UUID,
  permission_id,
  'system'
FROM permissions
WHERE permission_name IN ('replay:create', 'replay:read', 'replay:execute', 'replay:cancel', 'audit:read')
ON CONFLICT (role_id, permission_id) DO NOTHING;

-- Assign permissions to analyst role
INSERT INTO role_permissions (role_permission_id, role_id, permission_id, granted_by)
SELECT
  gen_random_uuid(),
  '22222222-2222-2222-2222-222222222222'::UUID,
  permission_id,
  'system'
FROM permissions
WHERE permission_name IN ('replay:create', 'replay:read', 'replay:execute', 'replay:cancel')
ON CONFLICT (role_id, permission_id) DO NOTHING;

-- Assign permissions to viewer role
INSERT INTO role_permissions (role_permission_id, role_id, permission_id, granted_by)
SELECT
  gen_random_uuid(),
  '33333333-3333-3333-3333-333333333333'::UUID,
  permission_id,
  'system'
FROM permissions
WHERE permission_name IN ('replay:read')
ON CONFLICT (role_id, permission_id) DO NOTHING;

COMMIT;
