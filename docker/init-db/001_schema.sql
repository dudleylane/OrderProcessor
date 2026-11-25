-- ============================================================
-- OMS PostgreSQL Schema
-- Auto-executed by postgres:16-alpine on first container boot.
-- Enum values match TypeScript enums in src/oms-frontend/src/types/enums.ts
-- and C++ enums in src/DataModelDef.h.
-- ============================================================

BEGIN;

-- ----------------------------------------------------------
-- Enum types
-- ----------------------------------------------------------

CREATE TYPE account_type AS ENUM (
    'PRINCIPAL',
    'AGENCY'
);

CREATE TYPE order_type AS ENUM (
    'MARKET',
    'LIMIT',
    'STOP',
    'STOPLIMIT'
);

CREATE TYPE exec_type AS ENUM (
    'NEW',
    'TRADE',
    'DFD',
    'CORRECT',
    'CANCEL',
    'REJECT',
    'REPLACE',
    'EXPIRED',
    'SUSPENDED',
    'STATUS',
    'RESTATED',
    'PEND_CANCEL',
    'PEND_REPLACE',
    'TRIGGERED'
);

CREATE TYPE currency AS ENUM (
    'USD',
    'EUR'
);

CREATE TYPE capacity AS ENUM (
    'AGENCY',
    'PRINCIPAL',
    'PROPRIETARY',
    'INDIVIDUAL',
    'RISKLESS_PRINCIPAL',
    'AGENT_FOR_ANOTHER_MEMBER'
);

CREATE TYPE time_in_force AS ENUM (
    'DAY',
    'GTD',
    'GTC',
    'FOK',
    'IOC',
    'OPG',
    'ATCLOSE'
);

CREATE TYPE side AS ENUM (
    'BUY',
    'SELL',
    'BUY_MINUS',
    'SELL_PLUS',
    'SELL_SHORT',
    'CROSS'
);

CREATE TYPE order_status AS ENUM (
    'RECEIVED_NEW',
    'PENDING_NEW',
    'PENDING_REPLACE',
    'NEW',
    'PARTIAL_FILL',
    'FILLED',
    'EXPIRED',
    'DONE_FOR_DAY',
    'SUSPENDED',
    'REPLACED',
    'CANCELED',
    'REJECTED'
);

CREATE TYPE settl_type AS ENUM (
    'REGULAR',
    'CASH',
    'NEXT_DAY',
    'T_PLUS_2',
    'T_PLUS_3',
    'T_PLUS_4',
    'T_PLUS_5',
    'SELLERS_OPTION',
    'WHEN_ISSUED',
    'T_PLUS_1',
    'BUYERS_OPTION',
    'SPECIAL_TRADE',
    'TENOR'
);

-- ----------------------------------------------------------
-- Helper: auto-update updated_at timestamp
-- ----------------------------------------------------------

CREATE OR REPLACE FUNCTION update_updated_at()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- ----------------------------------------------------------
-- Tables
-- ----------------------------------------------------------

-- Instruments (reference data — maps to C++ InstrumentEntry)
CREATE TABLE instruments (
    id                  BIGSERIAL PRIMARY KEY,
    symbol              VARCHAR(32)  NOT NULL UNIQUE,
    security_id         VARCHAR(64),
    security_id_source  VARCHAR(16)
);

ALTER TABLE instruments ADD COLUMN created_at TIMESTAMPTZ NOT NULL DEFAULT NOW();
ALTER TABLE instruments ADD COLUMN updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW();

CREATE TRIGGER trg_instruments_updated_at
    BEFORE UPDATE ON instruments
    FOR EACH ROW EXECUTE FUNCTION update_updated_at();

-- Accounts (reference data — maps to C++ AccountEntry)
CREATE TABLE accounts (
    id          BIGSERIAL PRIMARY KEY,
    account     VARCHAR(64)  NOT NULL UNIQUE,
    firm        VARCHAR(64)  NOT NULL,
    type        account_type NOT NULL,
    created_at  TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

CREATE TRIGGER trg_accounts_updated_at
    BEFORE UPDATE ON accounts
    FOR EACH ROW EXECUTE FUNCTION update_updated_at();

-- Clearing firms (reference data — maps to C++ ClearingEntry)
CREATE TABLE clearing_firms (
    id          BIGSERIAL PRIMARY KEY,
    firm        VARCHAR(64) NOT NULL UNIQUE,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TRIGGER trg_clearing_firms_updated_at
    BEFORE UPDATE ON clearing_firms
    FOR EACH ROW EXECUTE FUNCTION update_updated_at();

-- Orders (archive — maps to C++ OrderParams)
CREATE TABLE orders (
    id                  BIGSERIAL PRIMARY KEY,

    -- Engine composite ID (unique within the engine)
    order_id            BIGINT       NOT NULL,
    order_date          DATE         NOT NULL,

    -- Client/routing identifiers: TEXT, since a FIX order's ClOrdID is stored with its session's CompIDs (#71, #151)
    cl_order_id         TEXT,
    orig_cl_order_id    TEXT,
    source              TEXT,
    destination         TEXT,

    -- Order parameters
    side                side          NOT NULL,
    ord_type            order_type    NOT NULL,
    -- Prices are NUMERIC with no precision: (18,8) refused 1e10 or more, which the engine accepts (#538)
    price               NUMERIC,
    stop_px             NUMERIC,
    -- A STOP or STOPLIMIT order's stop was reached, and the order released (#223, #236)
    stop_triggered      BOOLEAN       NOT NULL DEFAULT false,
    -- Quantities are BIGINT: the engine's are unsigned 32-bit, which INTEGER can't hold above 2147483647 (#151)
    order_qty           BIGINT        NOT NULL,
    min_qty             BIGINT        NOT NULL DEFAULT 0,
    leaves_qty          BIGINT        NOT NULL DEFAULT 0,
    cum_qty             BIGINT        NOT NULL DEFAULT 0,
    avg_px              NUMERIC       NOT NULL DEFAULT 0,
    day_order_qty       BIGINT        NOT NULL DEFAULT 0,
    day_cum_qty         BIGINT        NOT NULL DEFAULT 0,
    day_avg_px          NUMERIC       NOT NULL DEFAULT 0,
    -- When the business day the day figures are for began, 17:00 New York (#244); NULL for none
    day_start           TIMESTAMPTZ,

    status              order_status  NOT NULL DEFAULT 'RECEIVED_NEW',
    time_in_force       time_in_force NOT NULL DEFAULT 'DAY',
    settl_type          settl_type,
    capacity            capacity      NOT NULL DEFAULT 'AGENCY',
    currency            currency      NOT NULL DEFAULT 'USD',

    -- Foreign keys to reference data
    instrument_id       BIGINT REFERENCES instruments(id),
    account_id          BIGINT REFERENCES accounts(id),
    clearing_firm_id    BIGINT REFERENCES clearing_firms(id),

    -- Timestamps
    expire_time         TIMESTAMPTZ,
    settl_date          DATE,
    created_at          TIMESTAMPTZ   NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ   NOT NULL DEFAULT NOW(),

    CONSTRAINT uq_engine_order UNIQUE (order_id, order_date)
);

CREATE TRIGGER trg_orders_updated_at
    BEFORE UPDATE ON orders
    FOR EACH ROW EXECUTE FUNCTION update_updated_at();

-- Executions (single-table inheritance via exec_type discriminator)
CREATE TABLE executions (
    id              BIGSERIAL PRIMARY KEY,
    order_id        BIGINT       NOT NULL REFERENCES orders(id),
    exec_type       exec_type    NOT NULL,

    -- Shared fields
    exec_id         VARCHAR(64),
    exec_time       TIMESTAMPTZ  NOT NULL DEFAULT NOW(),

    -- Trade-specific (exec_type = TRADE / CORRECT)
    last_px         NUMERIC(18,8),
    last_qty        BIGINT,
    trade_id        VARCHAR(64),

    -- Reject-specific (exec_type = REJECT)
    reject_reason   TEXT,

    -- Replace-specific (exec_type = REPLACE)
    new_price       NUMERIC(18,8),
    new_qty         BIGINT,

    -- Cancel-specific (exec_type = CANCEL)
    cancel_reason   TEXT,

    created_at      TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

-- ----------------------------------------------------------
-- Indexes (tuned for frontend blotter queries)
-- ----------------------------------------------------------

-- Order blotter: filter by status, symbol, time range
CREATE INDEX idx_orders_status      ON orders (status);
CREATE INDEX idx_orders_instrument  ON orders (instrument_id);
CREATE INDEX idx_orders_created_at  ON orders (created_at DESC);
CREATE INDEX idx_orders_side        ON orders (side);

-- Partial index: active orders only (most frequent query)
CREATE INDEX idx_orders_active ON orders (created_at DESC)
    WHERE status IN ('RECEIVED_NEW', 'PENDING_NEW', 'PENDING_REPLACE', 'NEW', 'PARTIAL_FILL');

-- Execution blotter: by order and time
CREATE INDEX idx_executions_order_id   ON executions (order_id);
CREATE INDEX idx_executions_exec_time  ON executions (exec_time DESC);
CREATE INDEX idx_executions_exec_type  ON executions (exec_type);

-- ----------------------------------------------------------
-- The schema's version (#590): a row a migration, each version the schema has been at. The server's writer reads the
-- highest when it connects, and refuses a schema of another version than its statements are written for
-- (PG::SCHEMA_VERSION). A volume made before this table mirrors as before, with a warning, until
-- scripts/pg-migrate.sh applies docker/migrations, which bring it here. A change to this file bumps the version, and
-- gets a migration of its own.
-- ----------------------------------------------------------

CREATE TABLE schema_version (
    version     INTEGER     PRIMARY KEY,
    applied_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
INSERT INTO schema_version (version) VALUES (1);

COMMIT;
