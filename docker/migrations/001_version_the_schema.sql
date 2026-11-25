-- ============================================================
-- Migration 1 (#590): versions a volume made before the schema was versioned, and brings its columns to
-- init-db/001_schema.sql's. Each statement leaves a volume that already has what it makes as it was, so a volume made
-- at any point before applies it whole. scripts/pg-migrate.sh applies it as the database's owner, in one transaction.
-- ============================================================

-- Quantities BIGINT and ClOrdIDs TEXT (#151): a volume made before held INTEGER and 64 characters
ALTER TABLE orders ALTER COLUMN order_qty TYPE BIGINT, ALTER COLUMN min_qty TYPE BIGINT,
    ALTER COLUMN leaves_qty TYPE BIGINT, ALTER COLUMN cum_qty TYPE BIGINT,
    ALTER COLUMN day_order_qty TYPE BIGINT, ALTER COLUMN day_cum_qty TYPE BIGINT,
    ALTER COLUMN cl_order_id TYPE TEXT, ALTER COLUMN orig_cl_order_id TYPE TEXT,
    ALTER COLUMN source TYPE TEXT, ALTER COLUMN destination TYPE TEXT;
ALTER TABLE executions ALTER COLUMN last_qty TYPE BIGINT, ALTER COLUMN new_qty TYPE BIGINT;

-- A stop's mark and the TRIGGERED execution type (#236)
ALTER TABLE orders ADD COLUMN IF NOT EXISTS stop_triggered BOOLEAN NOT NULL DEFAULT false;
ALTER TYPE exec_type ADD VALUE IF NOT EXISTS 'TRIGGERED';

-- The start of the business day an order's day figures are for (#244)
ALTER TABLE orders ADD COLUMN IF NOT EXISTS day_start TIMESTAMPTZ;

-- Prices NUMERIC with no precision (#538): NUMERIC(18,8) refused 1e10 or more
ALTER TABLE orders ALTER COLUMN price TYPE NUMERIC, ALTER COLUMN stop_px TYPE NUMERIC,
    ALTER COLUMN avg_px TYPE NUMERIC, ALTER COLUMN day_avg_px TYPE NUMERIC;

-- The version (#590), which the server's role reads as it connects, where the volume has the role (#568)
CREATE TABLE IF NOT EXISTS schema_version (
    version     INTEGER     PRIMARY KEY,
    applied_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
INSERT INTO schema_version (version) VALUES (1) ON CONFLICT (version) DO NOTHING;
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'oms_server') THEN
        GRANT SELECT ON schema_version TO oms_server;
    END IF;
END
$$;
