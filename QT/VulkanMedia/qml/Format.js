.pragma library

function bytes(n) {
    if (n === undefined || n === null)
        return "—"
    if (n < 1024)
        return n + " B"
    const units = ["KB", "MB", "GB", "TB"]
    let v = n / 1024
    let i = 0
    while (v >= 1024 && i < units.length - 1) {
        v /= 1024
        ++i
    }
    return (v < 10 ? v.toFixed(2) : v < 100 ? v.toFixed(1) : Math.round(v)) + " " + units[i]
}

function modified(date) {
    if (!date || isNaN(date.getTime()))
        return "—"
    return Qt.formatDateTime(date, "yyyy-MM-dd hh:mm")
}

// Player positions and durations arrive as milliseconds; -1 means "not known
// yet" (MediaPlayer reports that until the media is loaded).
function duration(ms) {
    if (ms === undefined || ms === null || ms < 0)
        return "—:—"
    const total = Math.floor(ms / 1000)
    const s = total % 60
    const m = Math.floor(total / 60) % 60
    const h = Math.floor(total / 3600)
    const ss = s < 10 ? "0" + s : "" + s
    if (h > 0)
        return h + ":" + (m < 10 ? "0" + m : "" + m) + ":" + ss
    return m + ":" + ss
}

function elideMiddle(text, max) {
    if (!text || text.length <= max)
        return text || ""
    const head = Math.ceil((max - 1) / 2)
    const tail = Math.floor((max - 1) / 2)
    return text.slice(0, head) + "…" + text.slice(text.length - tail)
}
