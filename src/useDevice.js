import { useEffect, useState } from 'react'
import { ref, onValue, update } from 'firebase/database'
import { db } from './firebase'

const OFFLINE_THRESHOLD_MS = 3 * 60 * 1000 // GSM polling is slow; 3 min of silence = offline

export function useDevice() {
  const [state, setState] = useState(null)
  const [command, setCommand] = useState(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    const stateRef = ref(db, 'device/state')
    const commandRef = ref(db, 'device/command')

    const unsubState = onValue(stateRef, (snap) => {
      setState(snap.val())
      setLoading(false)
    })
    const unsubCommand = onValue(commandRef, (snap) => {
      setCommand(snap.val())
    })

    return () => {
      unsubState()
      unsubCommand()
    }
  }, [])

  const isOnline = state?.lastSeen
    ? Date.now() - state.lastSeen < OFFLINE_THRESHOLD_MS
    : false

  const sendCommand = async (desiredState) => {
    await update(ref(db, 'device/command'), {
      desiredState,
      source: 'dashboard',
      issuedAt: Date.now(),
      ack: false,
    })
  }

  return { state, command, loading, isOnline, sendCommand }
}
