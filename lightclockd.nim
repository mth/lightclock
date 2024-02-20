import std/[net, parsecfg, strformat, streams, tables, times]

type
  InPacket = array[0..4, int64]
  OutPacket = array[0..4, int64]

var configFileName = "lightclock.conf"
const timeFmt = initTimeFormat("HH:mm")

proc readConfig(): Config =
  var f = configFileName.newFileStream fmRead
  if f == nil:
    echo "Cannot open config: ", configFileName
    return newConfig()
  defer: f.close
  f.loadConfig

proc getTime(section: OrderedTableRef[string, string], key: string, now: Time): int64 =
  let str = section.getOrDefault(key)
  if str.len == 0:
    echo &"Missing config field '{key}'"
    return 0
  let t = str.parse(timeFmt)
  let dt = now.local
  return dateTime(dt.year, dt.month, dt.monthDay, t.hour, t.minute, t.second,
                  t.nanosecond, t.timezone).toTime.toUnix

proc makeReply(request: InPacket, reply: var OutPacket): bool =
  let config = readConfig()
  let client = $request[0]
  echo &"client {client}"
  if client notin config:
    echo "Unknown client: ", client
    return false
  let clientConfig = config[client]
  echo &"client fade in start {$request[1].fromUnix}"
  echo &"client fade in end {$request[2].fromUnix}"
  echo &"client fade out start {$request[3].fromUnix}"
  echo &"client fade out end {$request[4].fromUnix}"
  reply[0] = 0x1234
  reply[1] = request[1]
  reply[2] = request[2]
  reply[3] = request[3]
  reply[4] = request[4]
  return true

proc handleRequests(socket: Socket) =
  while true:
    var clientAddr: string
    var clientPort: Port
    var data: string
    var input: InPacket
    let count = socket.recvFrom(data, input.sizeof, clientAddr, clientPort)
    if count != input.sizeof:
      echo &"Invalid input size: {count}, expected {input.sizeof} bytes"
      continue
    copyMem input.addr, data.cstring, input.sizeof
    var output: OutPacket
    if input.makeReply output:
      data.setLen output.sizeof
      copyMem data.cstring, output.addr, data.len
      echo &"sending reply to {clientPort}"
      socket.sendTo clientAddr, clientPort, data

echo readConfig()["1"].getTime("start", getTime())
let socket = newSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
socket.bindAddr Port(7171)
socket.handleRequests
