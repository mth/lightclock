import std/[net,strformat,times]

type
  InPacket = array[0..4, int64]
  OutPacket = array[0..4, int64]

let socket = newSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
socket.bindAddr Port(7171)

var client: Socket
var address = ""

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
  echo &"client fade in start {$input[1].fromUnix}"
  echo &"client fade in end {$input[2].fromUnix}"
  echo &"client fade out start {$input[3].fromUnix}"
  echo &"client fade out end {$input[4].fromUnix}"

  var output: OutPacket
  output[0] = 0x1234
  output[1] = input[1]
  output[2] = input[2]
  output[3] = input[3]
  output[4] = input[4]
  data.setLen output.sizeof
  copyMem data.cstring, output.addr, data.len
  echo &"sending reply to {clientPort}"
  socket.sendTo clientAddr, clientPort, data
