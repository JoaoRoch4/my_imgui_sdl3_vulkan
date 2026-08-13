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

function elideMiddle(text, max) {
    if (!text || text.length <= max)
        return text || ""
    const head = Math.ceil((max - 1) / 2)
    const tail = Math.floor((max - 1) / 2)
    return text.slice(0, head) + "…" + text.slice(text.length - tail)
}
