const WIDTH = 600
const HEIGHT = 160
const PADDING = 24

export default function VoltageChart({ entries }) {
  if (!entries.length) {
    return <div className="chart-empty">No voltage history yet</div>
  }

  const voltages = entries.map((e) => e.voltage ?? 0)
  const min = Math.min(...voltages)
  const max = Math.max(...voltages)
  const range = max - min || 1

  const points = entries.map((e, i) => {
    const x = PADDING + (i / Math.max(entries.length - 1, 1)) * (WIDTH - PADDING * 2)
    const y = HEIGHT - PADDING - ((e.voltage - min) / range) * (HEIGHT - PADDING * 2)
    return `${x},${y}`
  })

  const last = entries[entries.length - 1]

  return (
    <div className="chart-wrap">
      <svg viewBox={`0 0 ${WIDTH} ${HEIGHT}`} className="chart-svg" preserveAspectRatio="none">
        <polyline points={points.join(' ')} fill="none" stroke="var(--accent)" strokeWidth="2" />
      </svg>
      <div className="chart-legend">
        <span>min {min.toFixed(1)}V</span>
        <span>last {last.voltage?.toFixed(1)}V</span>
        <span>max {max.toFixed(1)}V</span>
      </div>
    </div>
  )
}
