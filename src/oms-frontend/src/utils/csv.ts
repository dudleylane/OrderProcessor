/** CSV export with proper escaping (fixes unescaped fields bug from FXswapX) */

export function escapeCsvField(value: unknown): string {
  const str = String(value ?? '');
  if (str.includes(',') || str.includes('"') || str.includes('\n') || str.includes('\r')) {
    return '"' + str.replace(/"/g, '""') + '"';
  }
  return str;
}

export function downloadCsv(
  filename: string,
  headers: string[],
  rows: (string | number | undefined)[][],
): void {
  const headerLine = headers.map(escapeCsvField).join(',');
  const dataLines = rows
    .map(row => row.map(escapeCsvField).join(','))
    .join('\n');

  const csv = headerLine + '\n' + dataLines;
  const blob = new Blob([csv], { type: 'text/csv;charset=utf-8;' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}
