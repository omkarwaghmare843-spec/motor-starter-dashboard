import { useEffect, useState } from 'react'
import { fetchStateHistory } from './thingspeak'

const POLL_INTERVAL_MS = 30 * 1000

export function useHistory(count = 50) {
  const [entries, setEntries] = useState([])

  useEffect(() => {
    let cancelled = false

    async function poll() {
      try {
        const list = await fetchStateHistory(count)
        if (!cancelled) setEntries(list)
      } catch (err) {
        console.error('ThingSpeak history poll failed:', err)
      }
    }

    poll()
    const interval = setInterval(poll, POLL_INTERVAL_MS)

    return () => {
      cancelled = true
      clearInterval(interval)
    }
  }, [count])

  return entries
}
