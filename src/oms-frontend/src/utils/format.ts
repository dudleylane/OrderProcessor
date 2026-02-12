/** Consolidates formatting utilities (fixes duplication from FXswapX) */

export function formatQuantity(n: number): string {
  const abs = Math.abs(n);
  if (abs >= 1_000_000) return `${(n / 1_000_000).toFixed(1)}M`;
  if (abs >= 1_000) return `${(n / 1_000).toFixed(0)}K`;
  return n.toString();
}

export function formatPrice(price: number, decimals = 4): string {
  return price.toFixed(decimals);
}

export function formatTime(ts: number): string {
  return new Date(ts).toLocaleTimeString('en-US', {
    hour12: false,
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  });
}

export function formatId(id: number): string {
  const s = id.toString();
  if (s.length <= 8) return s;
  return s.slice(0, 4) + '..' + s.slice(-4);
}
