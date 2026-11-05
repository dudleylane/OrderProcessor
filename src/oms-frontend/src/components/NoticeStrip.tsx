import type { Notice } from '../hooks/useOmsWebSocket';

interface NoticeStripProps {
  notices: Notice[];
  onDismiss: (id: number) => void;
  onDismissAll: () => void;
}

/**
 * The refusals the server sent and the requests the socket didn't take, newest first (#521). Each stays until it is
 * dismissed: one that hid itself after a few seconds could be missed, as the console's were.
 */
export function NoticeStrip({ notices, onDismiss, onDismissAll }: NoticeStripProps) {
  if (notices.length === 0) {
    return null;
  }

  return (
    <div className="border-b border-gray-800 bg-gray-900/60 px-6 py-2 text-sm">
      <div className="max-h-40 overflow-y-auto space-y-1">
        {notices.map(n => (
          <div key={n.id} className="flex items-center gap-3">
            <span className="text-xs text-gray-500 font-mono">{new Date(n.time).toLocaleTimeString()}</span>
            <span className={`flex-1 ${n.kind === 'not_sent' ? 'text-yellow-300' : 'text-red-300'}`}>{n.text}</span>
            <button
              onClick={() => onDismiss(n.id)}
              className="text-xs text-gray-500 hover:text-gray-300 transition-colors"
            >
              Dismiss
            </button>
          </div>
        ))}
      </div>
      {notices.length > 1 && (
        <div className="flex justify-end pt-1">
          <button
            onClick={onDismissAll}
            className="text-xs text-gray-500 hover:text-gray-300 transition-colors"
          >
            Dismiss all
          </button>
        </div>
      )}
    </div>
  );
}
