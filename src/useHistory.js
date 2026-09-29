import { useEffect, useState } from 'react'
import { ref, query, limitToLast, onValue } from 'firebase/database'
import { db } from './firebase'

export function useHistory(count = 50) {
  const [entries, setEntries] = useState([])

  useEffect(() => {
    const historyQuery = query(ref(db, 'device/history'), limitToLast(count))
    const unsub = onValue(historyQuery, (snap) => {
      const val = snap.val() || {}
      const list = Object.entries(val)
        .map(([id, entry]) => ({ id, ...entry }))
        .sort((a, b) => a.timestamp - b.timestamp)
      setEntries(list)
    })
    return () => unsub()
  }, [count])

  return entries
}
