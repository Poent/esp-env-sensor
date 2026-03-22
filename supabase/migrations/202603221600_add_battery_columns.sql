alter table public.readings
  add column if not exists battery_voltage_v double precision,
  add column if not exists battery_pct double precision;
