import { useEffect, useState } from 'react'
import { ref, onValue, update } from 'firebase/database'
import { db } from './firebase'

const OFFLINE_THRESHOLD_MS = 3 * 60 * 1000 // GSM polling is slow; 3 min of silence = offline

export function useDevice() {
  const [state, setState] = useState(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    const stateRef = ref(db, 'device/state')

    const unsubState = onValue(stateRef, (snap) => {
      setState(snap.val())
      setLoading(false)
    })

    return () => {
      unsubState()
    }
  }, [])

  const isOnline = state?.lastSeen
    ? Date.now() - state.lastSeen < OFFLINE_THRESHOLD_MS
    : false

  // Fire-and-forget, same as the LoRa remote: write the command and don't
  // wait for/track an ack. The device applies it on its next sync cycle
  // and reports the result back via device/state.
  const sendCommand = async (desiredState) => {
    await update(ref(db, 'device/command'), {
      desiredState,
      source: 'dashboard',
      issuedAt: Date.now(),
      ack: false,
    })
  }

  return { state, loading, isOnline, sendCommand }
}
