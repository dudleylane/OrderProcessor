#!/bin/sh
# The role the server connects as (#568): it reads and writes the rows it mirrors, and nothing else. The image's
# superuser, POSTGRES_USER, made the tables in 001_schema.sql, and the server used to connect as it. The image runs this
# once, as it initialises a new volume; an existing database gets the role by hand, as docker/README.md says.
set -eu
: "${OMS_DB_SERVER_PASSWORD:?set OMS_DB_SERVER_PASSWORD in docker/.env}"
# The password as a psql variable, which :'password' quotes as a literal whatever it holds
psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" \
    -v password="$OMS_DB_SERVER_PASSWORD" -v dbname="$POSTGRES_DB" <<'SQL'
CREATE ROLE oms_server LOGIN PASSWORD :'password';
GRANT CONNECT ON DATABASE :"dbname" TO oms_server;
GRANT USAGE ON SCHEMA public TO oms_server;
-- What PGRequestBuilder sends: upserts of the reference data and the orders, the reference data's ids looked up, and a
-- dropped order's row deleted (#597)
GRANT SELECT, INSERT, UPDATE ON instruments, accounts, clearing_firms, orders TO oms_server;
GRANT DELETE ON orders TO oms_server;
GRANT USAGE ON SEQUENCE instruments_id_seq, accounts_id_seq, clearing_firms_id_seq, orders_id_seq TO oms_server;
-- The schema's version, which the writer reads as it connects (#590). A volume made before it has none, until
-- scripts/pg-migrate.sh makes it, and grants it then.
DO $$
BEGIN
    IF to_regclass('schema_version') IS NOT NULL THEN
        GRANT SELECT ON schema_version TO oms_server;
    END IF;
END
$$;
SQL
