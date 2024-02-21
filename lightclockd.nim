import std/[net, os, parseutils, strformat, strutils, times]

type
  InPacket = array[0..4, int64]
  OutPacket = array[0..4, int64]

var configFileName = "lightclock.conf"

proc time(str: string, now: Time): int64 =
  let st = str.split(':', 3)
  var t = [0, 0, 0]
  for i in 0..<st.len():
    if st[i].parseInt(t[i]) != st[i].len:
      raise newException(ValueError, "Invalid time: " & str)
  let dt = now.local
  return dateTime(dt.year, dt.month, dt.monthDay, t[0], t[1], t[2], 0,
                  dt.timezone).toTime.toUnix

proc makeReply(request: InPacket, reply: var OutPacket): bool =
  let client = $request[0]
  try:
    for line in lines(configFileName):
      if line.startsWith('#'):
        continue
      let parts = line.split(' ')
      if parts.len >= 5 and parts[0] == client:
        echo "client ", client,
             " start-on=", request[1].fromUnix, " finish-off=", request[2].fromUnix,
             " start-off=", request[3].fromUnix, " finish-off=", request[4].fromUnix
        let curTime = getTime()
        reply[0] = curTime.toUnix
        for i in 1..4:
          reply[i] = parts[i].time(curTime)
        return true
    echo "Unknown client: ", client
  except IOError as e:
    echo "Configuration error: ", e.msg
  except ValueError as e:
    echo e.msg
  return false

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
      echo "sending reply to ", clientPort
      socket.sendTo clientAddr, clientPort, data

var port = 7117
var listen = ""
if paramCount() >= 1:
  configFileName = paramStr(1)
if paramCount() >= 2:
  discard paramStr(2).parseInt(port)
if paramCount() >= 3:
  listen = paramStr(3)
let socket = newSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
socket.bindAddr port.Port, listen
socket.handleRequests
