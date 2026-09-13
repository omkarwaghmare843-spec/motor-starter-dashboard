import { useEffect, useRef, useState } from 'react'
import { fetchLatestState, fetchLatestCommand, sendCommand as sendCommandToThingSpeak } from './thingspeak'

const OFFLINE_THRESHOLD_MS = 3 * 60 * 1000 // GSM polling is slow; 3 min of silence = offline
const POLL_INTERVAL_MS = 16 * 1000 // stay above ThingSpeak's ~15s per-channel rate limit

export function useDevice() {
  const [state, setState] = useState(null)
  const [command, setCommand] = useState(null)
  const [loading, setLoading] = useState(true)
  const pollRef = useRef(null)

  useEffect(() => {
    let cancelled = false

    async function poll() {
      try {
        const [latestState, latestCommand] = await Promise.all([
          fetchLatestState(),
          fetchLatestCommand(),
        ])
        if (cancelled) return
        if (latestState) setState(latestState)
        if (latestCommand) setCommand(latestCommand)
      } catch (err) {
        console.error('ThingSpeak poll failed:', err)
      } finally {
        if (!cancelled) setLoading(false)
      }
    }

    poll()
    pollRef.current = setInterval(poll, POLL_INTERVAL_MS)

    return () => {
      cancelled = true
      clearInterval(pollRef.current)
    }
  }, [])

  const isOnline = state?.receivedAt
    ? Date.now() - state.receivedAt < OFFLINE_THRESHOLD_MS
    : false

  const sendCommand = async (desiredState) => {
    await sendCommandToThingSpeak(desiredState)
    setCommand({ desiredState, ack: false, issuedAt: Math.floor(Date.now() / 1000) })
  }

  return {
    state: state ? { ...state, lastSeen: state.receivedAt } : null,
    command,
    loading,
    isOnline,
    sendCommand,
  }
}
