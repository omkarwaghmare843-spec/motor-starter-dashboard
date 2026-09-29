import { useState } from 'react'
import { useDevice } from './useDevice'
import { useHistory } from './useHistory'
import VoltageChart from './VoltageChart'
import { PowerIcon, SignalIcon, ClockIcon, HistoryIcon, RadioTowerIcon } from './icons'
import './index.css'

function formatTimeAgo(ts) {
  if (!ts) return 'never'
  const diff = Date.now() - ts
  const sec = Math.floor(diff / 1000)
  if (sec < 5) return 'just now'
  if (sec < 60) return `${sec}s ago`
  const min = Math.floor(sec / 60)
  if (min < 60) return `${min}m ago`
  const hr = Math.floor(min / 60)
  return `${hr}h ago`
}

function sourceLabel(source) {
  switch (source) {
    case 'dashboard':
      return 'Dashboard'
    case 'lora':
      return 'LoRa remote'
    case 'auto':
      return 'Auto-protection'
    default:
      return 'Unknown'
  }
}

export default function App() {
  const { state, loading, isOnline, sendCommand } = useDevice()
  const history = useHistory(50)
  const [sending, setSending] = useState(false)

  const motorStatus = state?.motorStatus ?? 'UNKNOWN'
  const isOn = motorStatus === 'ON'

  // Fire-and-forget, same as the LoRa remote: send the command and don't
  // wait for the device to ack it. The device's own periodic sync will
  // eventually update state.motorStatus once it applies the command.
  const handleToggle = async () => {
    setSending(true)
    try {
      await sendCommand(isOn ? 'OFF' : 'ON')
    } finally {
      setSending(false)
    }
  }

  return (
    <div className="page">
      <header className="topbar">
        <div className="brand">
          <span className="brand-mark">
            <PowerIcon width={20} height={20} />
          </span>
          <div>
            <h1>Motor Starter</h1>
            <p className="brand-sub">DOL Starter Control &amp; Telemetry</p>
          </div>
        </div>
        <div className={`badge ${isOnline ? 'badge-online' : 'badge-offline'}`}>
          <span className="dot" />
          {isOnline ? 'Device Online' : 'Device Offline'}
        </div>
      </header>

      {loading ? (
        <div className="loading-state">
          <div className="spinner" />
          <p className="muted">Connecting to device…</p>
        </div>
      ) : (
        <main className="grid">
          <section className="card control-card">
            <div className="card-head">
              <h2>Motor Control</h2>
              <span className={`status-pill status-${motorStatus.toLowerCase()}`}>
                <span className="status-dot" />
                {motorStatus}
              </span>
            </div>

            <button
              className={`toggle-btn ${isOn ? 'btn-stop' : 'btn-start'}`}
              onClick={handleToggle}
              disabled={sending}
            >
              <PowerIcon width={18} height={18} />
              {sending ? 'Sending command…' : isOn ? 'Stop Motor' : 'Start Motor'}
            </button>

            <div className="meta-row">
              <ClockIcon width={14} height={14} />
              <span>Last update {formatTimeAgo(state?.lastSeen)}</span>
            </div>
            {state?.gsmSignal != null && state.gsmSignal >= 0 && (
              <div className="meta-row">
                <SignalIcon width={14} height={14} />
                <span>GSM signal {state.gsmSignal}</span>
              </div>
            )}
          </section>

          <section className="card">
            <div className="card-head">
              <h2>Voltage</h2>
            </div>
            <div className="voltage-value">
              {state?.voltage != null ? state.voltage.toFixed(1) : '—'}
              <span className="unit">V</span>
            </div>
            <VoltageChart entries={history} />
          </section>

          <section className="card wide">
            <div className="card-head">
              <h2>
                <HistoryIcon width={16} height={16} className="head-icon" />
                Event Log
              </h2>
            </div>
            <div className="log-list">
              {history.length === 0 && (
                <div className="empty-state">
                  <RadioTowerIcon width={28} height={28} />
                  <p className="muted">No events recorded yet</p>
                  <p className="muted small">Events will appear once the device or LoRa remote reports in.</p>
                </div>
              )}
              {[...history].reverse().map((entry) => (
                <div key={entry.id} className="log-row">
                  <span className={`log-status status-${(entry.motorStatus ?? '').toLowerCase()}`}>
                    {entry.motorStatus}
                  </span>
                  <span className="log-voltage">{entry.voltage != null ? `${entry.voltage.toFixed(1)}V` : '—'}</span>
                  <span className="log-source">{sourceLabel(entry.source)}</span>
                  <span className="log-time">{new Date(entry.timestamp).toLocaleString()}</span>
                </div>
              ))}
            </div>
          </section>
        </main>
      )}

      <footer className="footer">
        <span>Motor Starter Dashboard</span>
        <span className="dot-sep">·</span>
        <span>ESP32 + LoRa SX1278 + A7670C GSM</span>
      </footer>
    </div>
  )
}
