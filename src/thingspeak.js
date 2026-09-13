const STATE_CHANNEL_ID = import.meta.env.VITE_TS_STATE_CHANNEL_ID
const STATE_READ_KEY = import.meta.env.VITE_TS_STATE_READ_KEY
const CMD_CHANNEL_ID = import.meta.env.VITE_TS_CMD_CHANNEL_ID
const CMD_WRITE_KEY = import.meta.env.VITE_TS_CMD_WRITE_KEY

const TS_BASE = 'https://api.thingspeak.com'

function toMotorStatus(field1) {
  return Number(field1) >= 1 ? 'ON' : 'OFF'
}

export async function fetchLatestState() {
  const url = `${TS_BASE}/channels/${STATE_CHANNEL_ID}/feeds/last.json?api_key=${STATE_READ_KEY}`
  const res = await fetch(url)
  if (!res.ok) throw new Error(`ThingSpeak state fetch failed: ${res.status}`)
  const feed = await res.json()
  if (!feed || feed.field1 == null) return null

  return {
    motorStatus: toMotorStatus(feed.field1),
    voltage: feed.field2 != null ? Number(feed.field2) : null,
    gsmSignal: feed.field3 != null ? Number(feed.field3) : null,
    receivedAt: new Date(feed.created_at).getTime(),
  }
}

export async function fetchStateHistory(count = 50) {
  const url = `${TS_BASE}/channels/${STATE_CHANNEL_ID}/feeds.json?api_key=${STATE_READ_KEY}&results=${count}`
  const res = await fetch(url)
  if (!res.ok) throw new Error(`ThingSpeak history fetch failed: ${res.status}`)
  const data = await res.json()
  const feeds = data?.feeds ?? []

  return feeds
    .filter((f) => f.field1 != null)
    .map((f) => ({
      id: f.entry_id,
      motorStatus: toMotorStatus(f.field1),
      voltage: f.field2 != null ? Number(f.field2) : null,
      timestamp: new Date(f.created_at).getTime(),
    }))
}

const CMD_READ_KEY = import.meta.env.VITE_TS_CMD_READ_KEY

export async function fetchLatestCommand() {
  const url = `${TS_BASE}/channels/${CMD_CHANNEL_ID}/feeds/last.json?api_key=${CMD_READ_KEY}`
  const res = await fetch(url)
  if (!res.ok) return null
  const feed = await res.json()
  if (!feed || feed.field1 == null) return null

  return {
    desiredState: toMotorStatus(feed.field1),
    ack: Number(feed.field3) >= 1,
    issuedAt: feed.field2 != null ? Number(feed.field2) : null,
  }
}

export async function sendCommand(desiredState) {
  const issuedAt = Math.floor(Date.now() / 1000)
  const field1 = desiredState === 'ON' ? 1 : 0
  const url = `${TS_BASE}/update?api_key=${CMD_WRITE_KEY}&field1=${field1}&field2=${issuedAt}&field3=0`
  const res = await fetch(url)
  if (!res.ok) throw new Error(`ThingSpeak command write failed: ${res.status}`)
  const entryId = await res.text()
  if (entryId === '0') {
    throw new Error('ThingSpeak rejected the write (rate limited or invalid key)')
  }
  return entryId
}
