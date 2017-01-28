#include <Nazara/Network/ENetHost.hpp>
#include <Nazara/Core/Clock.hpp>
#include <Nazara/Core/Endianness.hpp>
#include <Nazara/Core/OffsetOf.hpp>
#include <Nazara/Network/ENetPeer.hpp>
#include <Nazara/Network/NetPacket.hpp>
#include <Nazara/Network/Debug.hpp>

#define ENET_TIME_OVERFLOW 86400000

#define ENET_TIME_LESS(a, b) ((a) - (b) >= ENET_TIME_OVERFLOW)
#define ENET_TIME_GREATER(a, b) ((b) - (a) >= ENET_TIME_OVERFLOW)
#define ENET_TIME_LESS_EQUAL(a, b) (! ENET_TIME_GREATER (a, b))
#define ENET_TIME_GREATER_EQUAL(a, b) (! ENET_TIME_LESS (a, b))

#define ENET_TIME_DIFFERENCE(a, b) ((a) - (b) >= ENET_TIME_OVERFLOW ? (b) - (a) : (a) - (b))

namespace Nz
{
	/// Temporary
	template<typename T>
	T HostToNet(T value)
	{
		#ifdef NAZARA_LITTLE_ENDIAN
		return SwapBytes(value);
		#else
		return value;
		#endif
	}

	/// Temporary
	template<typename T>
	T NetToHost(T value)
	{
		#ifdef NAZARA_LITTLE_ENDIAN
		return SwapBytes(value);
		#else
		return value;
		#endif
	}

	namespace
	{
		static std::size_t s_commandSizes[ENetProtocolCommand_Count] =
		{
			0,
			sizeof(ENetProtocolAcknowledge),
			sizeof(ENetProtocolConnect),
			sizeof(ENetProtocolVerifyConnect),
			sizeof(ENetProtocolDisconnect),
			sizeof(ENetProtocolPing),
			sizeof(ENetProtocolSendReliable),
			sizeof(ENetProtocolSendUnreliable),
			sizeof(ENetProtocolSendFragment),
			sizeof(ENetProtocolSendUnsequenced),
			sizeof(ENetProtocolBandwidthLimit),
			sizeof(ENetProtocolThrottleConfigure),
			sizeof(ENetProtocolSendFragment)
		};
	}


	void ENetHost::Broadcast(UInt8 channelId, ENetPacketFlags flags, NetPacket&& packet)
	{
		ENetPacket* enetPacket = m_packetPool.New<ENetPacket>();
		enetPacket->flags = flags;
		enetPacket->data = std::move(packet);
		enetPacket->owner = &m_packetPool;

		for (ENetPeer& peer : m_peers)
		{
			if (peer.GetState() != ENetPeerState::Connected)
				continue;

			peer.Send(channelId, enetPacket);
		}
	}

	bool ENetHost::CheckEvents(ENetEvent* event)
	{
		if (!event)
			return false;

		event->type = ENetEventType::None;
		event->peer = nullptr;
		event->packet.Reset();

		return DispatchIncomingCommands(event);
	}

	ENetPeer* ENetHost::Connect(const IpAddress& remoteAddress, std::size_t channelCount, UInt32 data)
	{
		NazaraAssert(remoteAddress.IsValid(), "Invalid remote address");
		NazaraAssert(remoteAddress.GetPort() != 0, "Remote address has no port");

		std::size_t peerId;
		for (peerId = 0; peerId < m_peers.size(); ++peerId)
		{
			if (m_peers[peerId].GetState() == ENetPeerState::Disconnected)
				break;
		}

		if (peerId >= m_peers.size())
		{
			NazaraError("Insufficient peers");
			return nullptr;
		}

		m_channelLimit = Clamp<std::size_t>(channelCount, ENetConstants::ENetProtocol_MinimumChannelCount, ENetConstants::ENetProtocol_MaximumChannelCount);

		UInt32 windowSize;
		if (m_outgoingBandwidth == 0)
			windowSize = ENetProtocol_MaximumWindowSize;
		else
			windowSize = (m_outgoingBandwidth / ENetConstants::ENetPeer_WindowSizeScale) * ENetProtocol_MinimumWindowSize;

		ENetPeer& peer = m_peers[peerId];
		peer.InitOutgoing(channelCount, remoteAddress, ++m_randomSeed, windowSize);

		ENetProtocol command(ENetProtocolCommand_Connect | ENetProtocolFlag_Acknowledge, 0xFF);
		command.connect.channelCount = HostToNet(static_cast<UInt32>(channelCount));
		command.connect.connectID = peer.m_connectID;
		command.connect.data = HostToNet(data);
		command.connect.incomingBandwidth = HostToNet(m_incomingBandwidth);
		command.connect.incomingSessionID = peer.m_incomingSessionID;
		command.connect.mtu = HostToNet(peer.m_mtu);
		command.connect.outgoingBandwidth = HostToNet(m_outgoingBandwidth);
		command.connect.outgoingPeerID = HostToNet(peer.m_incomingPeerID);
		command.connect.outgoingSessionID = peer.m_outgoingSessionID;
		command.connect.packetThrottleAcceleration = HostToNet(peer.m_packetThrottleAcceleration);
		command.connect.packetThrottleDeceleration = HostToNet(peer.m_packetThrottleDeceleration);
		command.connect.packetThrottleInterval = HostToNet(peer.m_packetThrottleInterval);
		command.connect.windowSize = HostToNet(peer.m_windowSize);
		peer.QueueOutgoingCommand(command);

		return &peer;
	}

	ENetPeer* ENetHost::Connect(const String& hostName, NetProtocol protocol, const String& service, ResolveError* error, std::size_t channelCount, UInt32 data)
	{
		std::vector<HostnameInfo> results = IpAddress::ResolveHostname(protocol, hostName, service, error);
		if (results.empty())
			return nullptr;

		IpAddress hostnameAddress;
		for (const HostnameInfo& result : results)
		{
			if (!result.address)
				continue;

			if (result.socketType != SocketType_UDP)
				continue;

			hostnameAddress = result.address;
			break; //< Take first valid address
		}

		return Connect(hostnameAddress, channelCount, data);
	}

	bool ENetHost::Create(const IpAddress& address, std::size_t peerCount, std::size_t channelCount)
	{
		return Create(address, peerCount, channelCount, 0, 0);
	}

	bool ENetHost::Create(const IpAddress& address, std::size_t peerCount, std::size_t channelCount, UInt32 incomingBandwidth, UInt32 outgoingBandwidth)
	{
		NazaraAssert(address.IsValid(), "Invalid listening address");

		if (peerCount > ENetConstants::ENetProtocol_MaximumPeerId)
		{
			NazaraError("Peer count exceeds maximum peer count supported by protocol (" + String::Number(ENetConstants::ENetProtocol_MaximumPeerId) + ")");
			return false;
		}

		if (!InitSocket(address))
			return false;

		m_peers.reserve(peerCount);
		for (std::size_t i = 0; i < peerCount; ++i)
			m_peers.emplace_back(this, UInt16(i));

		m_address = address;
		m_randomSeed = *reinterpret_cast<UInt32*>(this);
		m_randomSeed += s_randomGenerator();
		m_randomSeed = (m_randomSeed << 16) | (m_randomSeed >> 16);
		m_channelLimit = Clamp<std::size_t>(channelCount, ENetConstants::ENetProtocol_MinimumChannelCount, ENetConstants::ENetProtocol_MaximumChannelCount);
		m_incomingBandwidth = incomingBandwidth;
		m_outgoingBandwidth = outgoingBandwidth;
		m_bandwidthThrottleEpoch = 0;
		m_recalculateBandwidthLimits = false;
		m_mtu = ENetConstants::ENetHost_DefaultMTU;
		m_commandCount = 0;
		m_bufferCount = 0;
		m_peerCount = peerCount;
		m_receivedAddress = IpAddress::AnyIpV4;
		m_receivedData = nullptr;
		m_receivedDataLength = 0;

		m_totalSentData = 0;
		m_totalSentPackets = 0;
		m_totalReceivedData = 0;
		m_totalReceivedPackets = 0;

		m_bandwidthLimitedPeers = 0;
		m_connectedPeers = 0;
		m_duplicatePeers = ENetConstants::ENetProtocol_MaximumPeerId;
		m_maximumPacketSize = ENetConstants::ENetHost_DefaultMaximumPacketSize;
		m_maximumWaitingData = ENetConstants::ENetHost_DefaultMaximumWaitingData;

		return true;
	}

	void ENetHost::Flush()
	{
		m_serviceTime = GetElapsedMilliseconds();

		SendOutgoingCommands(nullptr, 0);
	}

	int ENetHost::Service(ENetEvent* event, UInt32 timeout)
	{
		if (event)
		{
			event->type = ENetEventType::None;
			event->peer = nullptr;
			event->packet = nullptr;

			if (DispatchIncomingCommands(event))
				return 1;
		}

		m_serviceTime = GetElapsedMilliseconds();
		timeout += m_serviceTime;

		do
		{
			if (ENET_TIME_DIFFERENCE(m_serviceTime, m_bandwidthThrottleEpoch) >= ENetConstants::ENetHost_BandwidthThrottleInterval)
				ThrottleBandwidth();

			switch (SendOutgoingCommands(event, true))
			{
				case 1:
					return 1;

				case -1:
	#ifdef ENET_DEBUG
					perror("Error sending outgoing packets");
	#endif
					return -1;

				default:
					break;
			}

			switch (ReceiveIncomingCommands(event))
			{
			case 1:
				return 1;

			case -1:
#ifdef ENET_DEBUG
				perror("Error receiving incoming packets");
#endif

				return -1;

			default:
				break;
			}

			switch (SendOutgoingCommands(event, true))
			{
			case 1:
				return 1;

			case -1:
#ifdef ENET_DEBUG
				perror("Error sending outgoing packets");
#endif

				return -1;

			default:
				break;
			}

			if (event)
			{
				if (DispatchIncomingCommands(event))
					return 1;
			}

			if (ENET_TIME_GREATER_EQUAL(m_serviceTime, timeout))
				return 0;

			for (;;)
			{
				m_serviceTime = GetElapsedMilliseconds();

				if (ENET_TIME_GREATER_EQUAL(m_serviceTime, timeout))
					return 0;

				if (m_poller.Wait(ENET_TIME_DIFFERENCE(timeout, m_serviceTime)))
					break;
			}

			m_serviceTime = GetElapsedMilliseconds();
		}
		while (m_poller.IsReady(m_socket));

		return 0;
	}

	bool ENetHost::InitSocket(const IpAddress& address)
	{
		if (!m_socket.Create(address.GetProtocol()))
			return false;

		m_socket.EnableBlocking(false);
		m_socket.EnableBroadcasting(true);
		m_socket.SetReceiveBufferSize(ENetConstants::ENetHost_ReceiveBufferSize);
		m_socket.SetSendBufferSize(ENetConstants::ENetHost_SendBufferSize);

		if (!address.IsLoopback())
		{
			if (m_socket.Bind(address) != SocketState_Bound)
			{
				NazaraError("Failed to bind address " + address.ToString());
				return false;
			}
		}

		m_poller.RegisterSocket(m_socket);

		return true;
	}

	void ENetHost::AddToDispatchQueue(ENetPeer* peer)
	{
		m_dispatchQueue.UnboundedSet(peer->m_incomingPeerID);
	}

	void ENetHost::RemoveFromDispatchQueue(ENetPeer* peer)
	{
		m_dispatchQueue.UnboundedReset(peer->m_incomingPeerID);
	}

	bool ENetHost::DispatchIncomingCommands(ENetEvent* event)
	{
		for (std::size_t bit = m_dispatchQueue.FindFirst(); bit != m_dispatchQueue.npos; bit = m_dispatchQueue.FindNext(bit))
		{
			m_dispatchQueue.Reset(bit);

			ENetPeer& peer = m_peers[bit];
			switch (peer.GetState())
			{
				case ENetPeerState::ConnectionPending:
				case ENetPeerState::ConnectionSucceeded:
					peer.ChangeState(ENetPeerState::Connected);

					event->type = ENetEventType::Connect;
					event->peer = &peer;
					event->data = peer.m_eventData;
					return true;

				case ENetPeerState::Zombie:
					m_recalculateBandwidthLimits = true;

					event->type = ENetEventType::Disconnect;
					event->peer = &peer;
					event->data = peer.m_eventData;

					peer.Reset();
					return true;

				case ENetPeerState::Connected:
					if (peer.m_dispatchedCommands.empty())
						continue;

					if (!peer.Receive(&event->packet, &event->channelId))
						continue;

					event->type = ENetEventType::Receive;
					event->peer = &peer;

					if (!peer.m_dispatchedCommands.empty())
						AddToDispatchQueue(&peer);

					return true;

				default:
					break;
			}
		}

		return false;
	}

	bool ENetHost::HandleAcknowledge(ENetEvent* event, ENetPeer* peer, const ENetProtocol* command)
	{
		if (peer->GetState() == ENetPeerState::Disconnected || peer->GetState() == ENetPeerState::Zombie)
			return true;

		UInt32 receivedSentTime = NetToHost(command->acknowledge.receivedSentTime);
		receivedSentTime |= m_serviceTime & 0xFFFF0000;
		if ((receivedSentTime & 0x8000) > (m_serviceTime & 0x8000))
			receivedSentTime -= 0x10000;

		if (ENET_TIME_LESS(m_serviceTime, receivedSentTime))
			return true;

		peer->m_lastReceiveTime = m_serviceTime;
		peer->m_earliestTimeout = 0;

		UInt32 roundTripTime = ENET_TIME_DIFFERENCE(m_serviceTime, receivedSentTime);

		peer->Throttle(roundTripTime);

		peer->m_roundTripTimeVariance -= peer->m_roundTripTimeVariance / 4;

		if (roundTripTime >= peer->m_roundTripTime)
		{
			peer->m_roundTripTime += (roundTripTime - peer->m_roundTripTime) / 8;
			peer->m_roundTripTimeVariance += (roundTripTime - peer->m_roundTripTime) / 4;
		}
		else
		{
			peer->m_roundTripTime -= (peer->m_roundTripTime - roundTripTime) / 8;
			peer->m_roundTripTimeVariance += (peer->m_roundTripTime - roundTripTime) / 4;
		}

		if (peer->m_roundTripTime < peer->m_lowestRoundTripTime)
			peer->m_lowestRoundTripTime = peer->m_roundTripTime;

		if (peer->m_roundTripTimeVariance > peer->m_highestRoundTripTimeVariance)
			peer->m_highestRoundTripTimeVariance = peer->m_roundTripTimeVariance;

		if (peer->m_packetThrottleEpoch == 0 || ENET_TIME_DIFFERENCE(m_serviceTime, peer->m_packetThrottleEpoch) >= peer->m_packetThrottleInterval)
		{
			peer->m_lastRoundTripTime = peer->m_lowestRoundTripTime;
			peer->m_lastRoundTripTimeVariance = peer->m_highestRoundTripTimeVariance;
			peer->m_lowestRoundTripTime = peer->m_roundTripTime;
			peer->m_highestRoundTripTimeVariance = peer->m_roundTripTimeVariance;
			peer->m_packetThrottleEpoch = m_serviceTime;
		}

		UInt16 receivedReliableSequenceNumber = NetToHost(command->acknowledge.receivedReliableSequenceNumber);

		ENetProtocolCommand commandNumber = peer->RemoveSentReliableCommand(receivedReliableSequenceNumber, command->header.channelID);

		switch (peer->GetState())
		{
			case ENetPeerState::AcknowledgingConnect:
				if (commandNumber != ENetProtocolCommand_VerifyConnect)
					return false;

				NotifyConnect(peer, event);
				break;

			case ENetPeerState::Disconnecting:
				if (commandNumber != ENetProtocolCommand_Disconnect)
					return false;

				NotifyDisconnect(peer, event);
				break;

			case ENetPeerState::DisconnectLater:
				if (!peer->HasPendingCommands())
					peer->Disconnect(peer->m_eventData);

				break;

			default:
				break;
		}

		return true;
	}

	bool ENetHost::HandleBandwidthLimit(ENetPeer* peer, const ENetProtocol* command)
	{
		if (!peer->IsConnected())
			return false;

		if (peer->m_incomingBandwidth != 0)
			--m_bandwidthLimitedPeers;

		peer->m_incomingBandwidth = NetToHost(command->bandwidthLimit.incomingBandwidth);
		peer->m_outgoingBandwidth = NetToHost(command->bandwidthLimit.outgoingBandwidth);

		if (peer->m_incomingBandwidth != 0)
			++m_bandwidthLimitedPeers;

		if (peer->m_incomingBandwidth == 0 && m_outgoingBandwidth == 0)
			peer->m_windowSize = ENetConstants::ENetProtocol_MaximumWindowSize;
		else
		{
			if (peer->m_incomingBandwidth == 0 || m_outgoingBandwidth == 0)
				peer->m_windowSize = (std::max(peer->m_incomingBandwidth, m_outgoingBandwidth) / ENetConstants::ENetPeer_WindowSizeScale) * ENetConstants::ENetProtocol_MinimumWindowSize;
			else
				peer->m_windowSize = (std::min(peer->m_incomingBandwidth, m_outgoingBandwidth) / ENetConstants::ENetPeer_WindowSizeScale) * ENetConstants::ENetProtocol_MinimumWindowSize;

			peer->m_windowSize = Clamp<UInt32>(peer->m_windowSize, ENetConstants::ENetProtocol_MinimumWindowSize, ENetConstants::ENetProtocol_MaximumWindowSize);
		}

		return true;
	}

	ENetPeer* ENetHost::HandleConnect(ENetProtocolHeader* /*header*/, ENetProtocol* command)
	{
		UInt32 channelCount = NetToHost(command->connect.channelCount);

		if (channelCount < ENetProtocol_MinimumChannelCount || channelCount > ENetProtocol_MaximumChannelCount)
			return nullptr;

		std::size_t duplicatePeers = 0;
		ENetPeer* peer = nullptr;
		for (ENetPeer& currentPeer : m_peers)
		{
			if (currentPeer.GetState() == ENetPeerState::Disconnected)
			{
				if (!peer)
					peer = &currentPeer;
			}
			else if (currentPeer.GetState() != ENetPeerState::Connecting)
			{
				// Compare users without comparing their port
				IpAddress first(currentPeer.m_address);
				first.SetPort(0);

				IpAddress second(m_receivedAddress);
				second.SetPort(0);

				if (first == second)
				{
					if (currentPeer.m_address.GetPort() == m_receivedAddress.GetPort() && currentPeer.m_connectID == command->connect.connectID)
						return nullptr;

					++duplicatePeers;
				}
			}
		}

		if (!peer || duplicatePeers >= m_duplicatePeers)
			return nullptr;

		channelCount = std::min<UInt32>(channelCount, m_channelLimit);

		peer->InitIncoming(channelCount, m_receivedAddress, command->connect);

		UInt32 windowSize;
		if (m_incomingBandwidth == 0)
			windowSize = ENetConstants::ENetProtocol_MaximumWindowSize;
		else
			windowSize = (m_incomingBandwidth / ENetConstants::ENetPeer_WindowSizeScale) * ENetConstants::ENetProtocol_MinimumWindowSize;

		windowSize = std::max(windowSize, NetToHost(command->connect.windowSize));
		windowSize = Clamp<UInt32>(windowSize, ENetConstants::ENetProtocol_MinimumWindowSize, ENetConstants::ENetProtocol_MaximumWindowSize);

		ENetProtocol verifyCommand(ENetProtocolCommand_VerifyConnect | ENetProtocolFlag_Acknowledge, 0xFF);
		verifyCommand.verifyConnect.outgoingPeerID             = HostToNet(peer->m_incomingPeerID);
		verifyCommand.verifyConnect.incomingSessionID          = peer->m_outgoingSessionID;
		verifyCommand.verifyConnect.outgoingSessionID          = peer->m_incomingSessionID;
		verifyCommand.verifyConnect.mtu                        = HostToNet(peer->m_mtu);
		verifyCommand.verifyConnect.windowSize                 = HostToNet(windowSize);
		verifyCommand.verifyConnect.channelCount               = HostToNet(channelCount);
		verifyCommand.verifyConnect.incomingBandwidth          = HostToNet(m_incomingBandwidth);
		verifyCommand.verifyConnect.outgoingBandwidth          = HostToNet(m_outgoingBandwidth);
		verifyCommand.verifyConnect.packetThrottleInterval     = HostToNet(peer->m_packetThrottleInterval);
		verifyCommand.verifyConnect.packetThrottleAcceleration = HostToNet(peer->m_packetThrottleAcceleration);
		verifyCommand.verifyConnect.packetThrottleDeceleration = HostToNet(peer->m_packetThrottleDeceleration);
		verifyCommand.verifyConnect.connectID = peer->m_connectID;
		peer->QueueOutgoingCommand(verifyCommand);

		return peer;
	}

	bool ENetHost::HandleDisconnect(ENetPeer* peer, const ENetProtocol * command)
	{
		if (peer->GetState() == ENetPeerState::Disconnected || peer->GetState() == ENetPeerState::Zombie || peer->GetState() == ENetPeerState::AcknowledgingDisconnect)
			return true;

		peer->ResetQueues();

		if (peer->GetState() == ENetPeerState::ConnectionSucceeded || peer->GetState() == ENetPeerState::Disconnecting || peer->GetState() == ENetPeerState::Connecting)
			peer->DispatchState(ENetPeerState::Zombie);
		else
		{
			if (!peer->IsConnected())
			{
				if (peer->GetState() == ENetPeerState::ConnectionPending)
					m_recalculateBandwidthLimits = true;

				peer->Reset();
			}
			else
				if (command->header.command & ENetProtocolFlag_Acknowledge)
					peer->ChangeState(ENetPeerState::AcknowledgingDisconnect);
				else
					peer->DispatchState(ENetPeerState::Zombie);
		}

		if (peer->GetState() != ENetPeerState::Disconnected)
			peer->m_eventData = NetToHost(command->disconnect.data);

		return true;
	}

	bool ENetHost::HandleIncomingCommands(ENetEvent* event)
	{
		if (m_receivedDataLength < NazaraOffsetOf(ENetProtocolHeader, sentTime))
			return false;

		ENetProtocolHeader* header = reinterpret_cast<ENetProtocolHeader*>(m_receivedData);

		UInt16 peerID = NetToHost(header->peerID);
		UInt8  sessionID = (peerID & ENetProtocolHeaderSessionMask) >> ENetProtocolHeaderSessionShift;
		UInt16 flags = peerID & ENetProtocolHeaderFlag_Mask;
		peerID &= ~(ENetProtocolHeaderFlag_Mask | ENetProtocolHeaderSessionMask);

		std::size_t headerSize = (flags & ENetProtocolHeaderFlag_SentTime ? sizeof(ENetProtocolHeader) : (size_t) & ((ENetProtocolHeader *)0)->sentTime);

		ENetPeer* peer;
		if (peerID == ENetConstants::ENetProtocol_MaximumPeerId)
			peer = nullptr;
		else
		{
			if (peerID >= m_peers.size())
				return false;
			else
			{
				peer = &m_peers[peerID];

				if (peer->GetState() == ENetPeerState::Disconnected || peer->GetState() == ENetPeerState::Zombie)
					return false;

				if (m_receivedAddress != peer->m_address && peer->m_address != IpAddress::BroadcastIpV4)
					return false;

				if (peer->m_outgoingPeerID < ENetConstants::ENetProtocol_MaximumPeerId && sessionID != peer->m_incomingSessionID)
					return false;
			}
		}

		// Compression handling

		// Checksum

		if (peer)
		{
			peer->m_address = m_receivedAddress;
			peer->m_incomingDataTotal += m_receivedDataLength;
		}

		auto commandError = [&]() -> bool
		{
			if (event && event->type != ENetEventType::None)
				return true;

			return false;
		};

		UInt8* currentData = m_receivedData + headerSize;

		while (currentData < &m_receivedData[m_receivedDataLength])
		{
			ENetProtocol* command = reinterpret_cast<ENetProtocol*>(currentData);

			if (currentData + sizeof(ENetProtocolCommandHeader) > &m_receivedData[m_receivedDataLength])
				break;

			UInt8 commandNumber = command->header.command & ENetProtocolCommand_Mask;
			if (commandNumber >= ENetProtocolCommand_Count)
				break;

			std::size_t commandSize = s_commandSizes[commandNumber];
			if (commandSize == 0 || currentData + commandSize > &m_receivedData[m_receivedDataLength])
				break;

			currentData += commandSize;

			if (!peer && commandNumber != ENetProtocolCommand_Connect)
				break;

			command->header.reliableSequenceNumber = NetToHost(command->header.reliableSequenceNumber);

			switch (commandNumber)
			{
				case ENetProtocolCommand_Acknowledge:
					if (!HandleAcknowledge(event, peer, command))
						return commandError();

					break;

				case ENetProtocolCommand_Connect:
					if (peer)
						return commandError();

					peer = HandleConnect(header, command);
					if (!peer)
						return commandError();

					break;

				case ENetProtocolCommand_VerifyConnect:
					if (!HandleVerifyConnect(event, peer, command))
						return commandError();

					break;

				case ENetProtocolCommand_Disconnect:
					if (!HandleDisconnect(peer, command))
						return commandError();

					break;

				case ENetProtocolCommand_Ping:
					if (!HandlePing(peer, command))
						return commandError();

					break;

				case ENetProtocolCommand_SendReliable:
					if (!HandleSendReliable(peer, command, &currentData))
						return commandError();

					break;

				case ENetProtocolCommand_SendUnreliable:
					if (!HandleSendUnreliable(peer, command, &currentData))
						return commandError();

					break;

				case ENetProtocolCommand_SendUnsequenced:
					if (!HandleSendUnsequenced(peer, command, &currentData))
						return commandError();

					break;

				case ENetProtocolCommand_SendFragment:
					if (!HandleSendFragment(peer, command, &currentData))
						return commandError();

					break;

				case ENetProtocolCommand_BandwidthLimit:
					if (!HandleBandwidthLimit(peer, command))
						return commandError();

					break;

				case ENetProtocolCommand_ThrottleConfigure:
					if (!HandleThrottleConfigure(peer, command))
						return commandError();

					break;

				case ENetProtocolCommand_SendUnreliableFragment:
					if (!HandleSendUnreliableFragment(peer, command, &currentData))
						return commandError();

					break;

				default:
					return commandError();
			}

			if (peer && (command->header.command & ENetProtocolFlag_Acknowledge) != 0)
			{
				UInt16 sentTime;

				if (!(flags & ENetProtocolHeaderFlag_SentTime))
					break;

				sentTime = NetToHost(header->sentTime);

				switch (peer->GetState())
				{
					case ENetPeerState::Disconnecting:
					case ENetPeerState::AcknowledgingConnect:
					case ENetPeerState::Disconnected:
					case ENetPeerState::Zombie:
						break;

					case ENetPeerState::AcknowledgingDisconnect:
						if ((command->header.command & ENetProtocolCommand_Mask) == ENetProtocolCommand_Disconnect)
							peer->QueueAcknowledgement(command, sentTime);
						break;

					default:
						peer->QueueAcknowledgement(command, sentTime);
						break;
				}
			}
		}

		return commandError();
	}

	bool ENetHost::HandlePing(ENetPeer* peer, const ENetProtocol* /*command*/)
	{
		if (!peer->IsConnected())
			return false;

		return true;
	}

	bool ENetHost::HandleSendFragment(ENetPeer* peer, const ENetProtocol* command, UInt8** currentData)
	{
		if (command->header.channelID >= peer->m_channels.size() || !peer->IsConnected())
			return false;

		UInt16 fragmentLength = NetToHost(command->sendFragment.dataLength);
		*currentData += fragmentLength;
		if (fragmentLength >= m_maximumPacketSize || *currentData < m_receivedData || *currentData > &m_receivedData[m_receivedDataLength])
			return false;

		ENetPeer::Channel& channel = peer->m_channels[command->header.channelID];
		UInt32 startSequenceNumber = NetToHost(command->sendFragment.startSequenceNumber);
		UInt16 startWindow = startSequenceNumber / ENetConstants::ENetPeer_ReliableWindowSize;
		UInt16 currentWindow = channel.incomingReliableSequenceNumber / ENetConstants::ENetPeer_ReliableWindowSize;

		if (startSequenceNumber < channel.incomingReliableSequenceNumber)
			startWindow += ENetConstants::ENetPeer_ReliableWindows;

		if (startWindow < currentWindow || startWindow >= currentWindow + ENetConstants::ENetPeer_FreeReliableWindows - 1)
			return true;

		UInt32 fragmentNumber = NetToHost(command->sendFragment.fragmentNumber);
		UInt32 fragmentCount  = NetToHost(command->sendFragment.fragmentCount);
		UInt32 fragmentOffset = NetToHost(command->sendFragment.fragmentOffset);
		UInt32 totalLength    = NetToHost(command->sendFragment.totalLength);

		if (fragmentCount > ENetConstants::ENetProtocol_MaximumFragmentCount || fragmentNumber >= fragmentCount || totalLength > m_maximumPacketSize ||
			fragmentOffset >= totalLength || fragmentLength > totalLength - fragmentOffset)
			return false;

		ENetPeer::IncomingCommmand* startCommand = nullptr;
		for (auto currentCommand = channel.incomingReliableCommands.rbegin(); currentCommand != channel.incomingReliableCommands.rend(); ++currentCommand)
		{
			ENetPeer::IncomingCommmand& incomingCommand = *currentCommand;

			if (startSequenceNumber >= channel.incomingReliableSequenceNumber)
			{
				if (incomingCommand.reliableSequenceNumber < channel.incomingReliableSequenceNumber)
					continue;
			}
			else if (incomingCommand.reliableSequenceNumber >= channel.incomingReliableSequenceNumber)
				break;

			if (incomingCommand.reliableSequenceNumber <= startSequenceNumber)
			{
				if (incomingCommand.reliableSequenceNumber < startSequenceNumber)
					break;

				if ((incomingCommand.command.header.command & ENetProtocolCommand_Mask) != ENetProtocolCommand_SendFragment ||
					totalLength != incomingCommand.packet->data.GetDataSize() || fragmentCount != incomingCommand.fragments.GetSize())
					return false;

				startCommand = &incomingCommand;
				break;
			}
		}

		if (startCommand)
		{
			ENetProtocol hostCommand = *command;
			hostCommand.header.reliableSequenceNumber = startSequenceNumber;

			if (!peer->QueueIncomingCommand(hostCommand, nullptr, totalLength, ENetPacketFlag_Reliable, fragmentCount))
				return false;
		}

		if (!startCommand->fragments.Test(fragmentNumber))
		{
			--startCommand->fragmentsRemaining;

			startCommand->fragments.Set(fragmentNumber, true);

			if (fragmentOffset + fragmentLength > startCommand->packet->data.GetDataSize())
				fragmentLength = startCommand->packet->data.GetDataSize() - fragmentOffset;

			std::memcpy(startCommand->packet->data.GetData() + Nz::NetPacket::HeaderSize + fragmentOffset, reinterpret_cast<const UInt8*>(command) + sizeof(ENetProtocolSendFragment), fragmentLength);

			if (startCommand->fragmentsRemaining <= 0)
				peer->DispatchIncomingReliableCommands(channel);
		}

		return false;
	}

	bool ENetHost::HandleSendReliable(ENetPeer* peer, const ENetProtocol* command, UInt8** currentData)
	{
		if (command->header.channelID >= peer->m_channels.size() || !peer->IsConnected())
			return false;

		UInt16 dataLength = NetToHost(command->sendReliable.dataLength);
		*currentData += dataLength;
		if (dataLength >= m_maximumPacketSize || *currentData < m_receivedData || *currentData > &m_receivedData[m_receivedDataLength])
			return false;

		if (!peer->QueueIncomingCommand(*command, reinterpret_cast<const UInt8*>(command) + sizeof(ENetProtocolSendReliable), dataLength, ENetPacketFlag_Reliable, 0))
			return false;

		return true;
	}

	bool ENetHost::HandleSendUnreliable(ENetPeer * peer, const ENetProtocol * command, UInt8 ** currentData)
	{
		if (command->header.channelID >= peer->m_channels.size() || !peer->IsConnected())
			return false;

		UInt16 dataLength = NetToHost(command->sendUnreliable.dataLength);
		*currentData += dataLength;
		if (dataLength >= m_maximumPacketSize || *currentData < m_receivedData || *currentData > &m_receivedData[m_receivedDataLength])
			return false;

		if (!peer->QueueIncomingCommand(*command, reinterpret_cast<const UInt8*>(command) + sizeof(ENetProtocolSendUnreliable), dataLength, 0, 0))
			return false;

		return true;
	}

	bool ENetHost::HandleSendUnreliableFragment(ENetPeer* peer, const ENetProtocol* command, UInt8** currentData)
	{
		if (command->header.channelID >= peer->m_channels.size() || !peer->IsConnected())
			return false;

		UInt16 fragmentLength = NetToHost(command->sendFragment.dataLength);
		*currentData += fragmentLength;
		if (fragmentLength >= m_maximumPacketSize || *currentData < m_receivedData || *currentData > &m_receivedData[m_receivedDataLength])
			return false;

		ENetPeer::Channel& channel = peer->m_channels[command->header.channelID];
		UInt32 reliableSequenceNumber = command->header.reliableSequenceNumber;
		UInt32 startSequenceNumber = NetToHost(command->sendFragment.startSequenceNumber);

		UInt16 reliableWindow = reliableSequenceNumber                 / ENetConstants::ENetPeer_ReliableWindowSize;
		UInt16 currentWindow  = channel.incomingReliableSequenceNumber / ENetConstants::ENetPeer_ReliableWindowSize;

		if (startSequenceNumber < channel.incomingReliableSequenceNumber)
			reliableWindow += ENetConstants::ENetPeer_ReliableWindows;

		if (reliableWindow < currentWindow || reliableWindow >= currentWindow + ENetConstants::ENetPeer_FreeReliableWindows - 1)
			return true;

		if (reliableSequenceNumber == channel.incomingReliableSequenceNumber && startSequenceNumber <= channel.incomingUnreliableSequenceNumber)
			return true;

		UInt32 fragmentNumber = NetToHost(command->sendFragment.fragmentNumber);
		UInt32 fragmentCount  = NetToHost(command->sendFragment.fragmentCount);
		UInt32 fragmentOffset = NetToHost(command->sendFragment.fragmentOffset);
		UInt32 totalLength    = NetToHost(command->sendFragment.totalLength);

		if (fragmentCount > ENetConstants::ENetProtocol_MaximumFragmentCount || fragmentNumber >= fragmentCount || totalLength > m_maximumPacketSize ||
			fragmentOffset >= totalLength || fragmentLength > totalLength - fragmentOffset)
			return false;

		ENetPeer::IncomingCommmand* startCommand = nullptr;
		for (auto currentCommand = channel.incomingUnreliableCommands.rbegin(); currentCommand != channel.incomingUnreliableCommands.rend(); ++currentCommand)
		{
			ENetPeer::IncomingCommmand& incomingCommand = *currentCommand;

			if (startSequenceNumber >= channel.incomingReliableSequenceNumber)
			{
				if (incomingCommand.reliableSequenceNumber < channel.incomingReliableSequenceNumber)
					continue;
			}
			else if (incomingCommand.reliableSequenceNumber >= channel.incomingReliableSequenceNumber)
				break;

			if (incomingCommand.reliableSequenceNumber < reliableSequenceNumber)
				break;

			if (incomingCommand.reliableSequenceNumber > reliableSequenceNumber)
				continue;

			if (incomingCommand.unreliableSequenceNumber <= startSequenceNumber)
			{
				if (incomingCommand.unreliableSequenceNumber < startSequenceNumber)
					break;

				if ((incomingCommand.command.header.command & ENetProtocolCommand_Mask) != ENetProtocolCommand_SendUnreliableFragment ||
					totalLength != incomingCommand.packet->data.GetDataSize() || fragmentCount != incomingCommand.fragments.GetSize())
					return false;

				startCommand = &incomingCommand;
				break;
			}
		}

		if (startCommand)
		{
			if (!peer->QueueIncomingCommand(*command, nullptr, totalLength, ENetPacketFlag_UnreliableFragment, fragmentCount))
				return false;
		}

		if (!startCommand->fragments.Test(fragmentNumber))
		{
			--startCommand->fragmentsRemaining;

			startCommand->fragments.Set(fragmentNumber, true);

			if (fragmentOffset + fragmentLength > startCommand->packet->data.GetDataSize())
				fragmentLength = startCommand->packet->data.GetDataSize() - fragmentOffset;

			std::memcpy(startCommand->packet->data.GetData() + Nz::NetPacket::HeaderSize + fragmentOffset, reinterpret_cast<const UInt8*>(command) + sizeof(ENetProtocolSendFragment), fragmentLength);

			if (startCommand->fragmentsRemaining <= 0)
				peer->DispatchIncomingUnreliableCommands(channel);
		}

		return true;
	}

	bool ENetHost::HandleSendUnsequenced(ENetPeer* peer, const ENetProtocol* command, UInt8** currentData)
	{
		if (command->header.channelID >= peer->m_channels.size() || !peer->IsConnected())
			return false;

		std::size_t dataLength = NetToHost(command->sendUnsequenced.dataLength);
		*currentData += dataLength;
		if (dataLength >= m_maximumPacketSize || *currentData < m_receivedData || *currentData > &m_receivedData[m_receivedDataLength])
			return false;

		UInt32 unsequencedGroup = NetToHost(command->sendUnsequenced.unsequencedGroup);
		UInt32 index = unsequencedGroup % ENetConstants::ENetPeer_UnsequencedWindowSize;

		if (unsequencedGroup < peer->m_incomingUnsequencedGroup)
			unsequencedGroup += 0x10000;

		if (unsequencedGroup >= static_cast<UInt32>(peer->m_incomingUnsequencedGroup) + ENetConstants::ENetPeer_UnsequencedWindows * ENetConstants::ENetPeer_UnsequencedWindowSize)
			return true;

		unsequencedGroup &= 0xFFFF;

		if (unsequencedGroup - index != peer->m_incomingUnsequencedGroup)
		{
			peer->m_incomingUnsequencedGroup = unsequencedGroup - index;

			peer->m_unsequencedWindow.fill(0);
		}
		else if (peer->m_unsequencedWindow[index / 32] & (1 << (index % 32)))
			return true;

		if (!peer->QueueIncomingCommand(*command, reinterpret_cast<const UInt8*>(command) + sizeof(ENetProtocolSendUnsequenced), dataLength, ENetPacketFlag_Unsequenced, 0))
			return false;

		peer->m_unsequencedWindow[index / 32] |= 1 << (index % 32);

		return true;
	}

	bool ENetHost::HandleThrottleConfigure(ENetPeer* peer, const ENetProtocol* command)
	{
		if (!peer->IsConnected())
			return false;

		peer->m_packetThrottleInterval     = NetToHost(command->throttleConfigure.packetThrottleInterval);
		peer->m_packetThrottleAcceleration = NetToHost(command->throttleConfigure.packetThrottleAcceleration);
		peer->m_packetThrottleDeceleration = NetToHost(command->throttleConfigure.packetThrottleDeceleration);

		return true;
	}

	bool ENetHost::HandleVerifyConnect(ENetEvent* event, ENetPeer* peer, ENetProtocol* command)
	{
		if (peer->GetState() != ENetPeerState::Connecting)
			return false;

		UInt32 channelCount = NetToHost(command->verifyConnect.channelCount);

		if (channelCount < ENetConstants::ENetProtocol_MinimumChannelCount || channelCount > ENetConstants::ENetProtocol_MaximumChannelCount ||
			NetToHost(command->verifyConnect.packetThrottleInterval) != peer->m_packetThrottleInterval ||
			NetToHost(command->verifyConnect.packetThrottleAcceleration) != peer->m_packetThrottleAcceleration ||
			NetToHost(command->verifyConnect.packetThrottleDeceleration) != peer->m_packetThrottleDeceleration ||
			command->verifyConnect.connectID != peer->m_connectID)
		{
			peer->m_eventData = 0;

			peer->DispatchState(ENetPeerState::Zombie);

			return false;
		}

		peer->RemoveSentReliableCommand(1, 0xFF);

		if (channelCount < peer->m_channels.size())
			peer->m_channels.resize(channelCount);

		peer->m_outgoingPeerID = NetToHost(command->verifyConnect.outgoingPeerID);
		peer->m_incomingSessionID = command->verifyConnect.incomingSessionID;
		peer->m_outgoingSessionID = command->verifyConnect.outgoingSessionID;

		UInt32 mtu = Clamp<UInt32>(NetToHost(command->verifyConnect.mtu), ENetConstants::ENetProtocol_MinimumMTU, ENetConstants::ENetProtocol_MaximumMTU);
		peer->m_mtu = std::min(peer->m_mtu, mtu);

		UInt32 windowSize = Clamp<UInt32>(NetToHost(command->verifyConnect.windowSize), ENetConstants::ENetProtocol_MinimumWindowSize, ENetConstants::ENetProtocol_MaximumWindowSize);
		peer->m_windowSize = std::min(peer->m_windowSize, windowSize);

		peer->m_incomingBandwidth = NetToHost(command->verifyConnect.incomingBandwidth);
		peer->m_outgoingBandwidth = NetToHost(command->verifyConnect.outgoingBandwidth);

		NotifyConnect(peer, event);
		return true;
	}

	int ENetHost::ReceiveIncomingCommands(ENetEvent* event)
	{
		for (unsigned int i = 0; i < 256; ++i)
		{
			NetPacket packet;

			std::size_t receivedLength;
			if (!m_socket.Receive(m_packetData[0].data(), m_packetData[0].size(), &m_receivedAddress, &receivedLength))
				return -1; //< Error

			if (receivedLength == 0)
				return 0;

			m_receivedData = m_packetData[0].data();
			m_receivedDataLength = receivedLength;

			m_totalReceivedData += receivedLength;
			m_totalReceivedPackets++;

			// Intercept

			if (HandleIncomingCommands(event))
				return 1;
		}

		return -1;
	}

	void ENetHost::NotifyConnect(ENetPeer* peer, ENetEvent* event)
	{
		m_recalculateBandwidthLimits = true;

		if (event)
		{
			peer->ChangeState(ENetPeerState::Connected);

			event->type = ENetEventType::Connect;
			event->peer = peer;
			event->data = peer->m_eventData;
		}
		else
			peer->DispatchState(peer->GetState() == ENetPeerState::Connecting ? ENetPeerState::ConnectionSucceeded : ENetPeerState::ConnectionPending);
	}

	void ENetHost::NotifyDisconnect(ENetPeer* peer, ENetEvent* event)
	{
		if (peer->GetState() >= ENetPeerState::ConnectionPending)
			m_recalculateBandwidthLimits = true;

		if (peer->GetState() != ENetPeerState::Connecting && (peer->GetState() < ENetPeerState::ConnectionSucceeded))
			peer->Reset();
		else if (event)
		{
			event->type = ENetEventType::Disconnect;
			event->peer = peer;
			event->data = peer->m_eventData;

			peer->Reset();
		}
		else
		{
			peer->m_eventData = 0;

			peer->DispatchState(ENetPeerState::Zombie);
		}
	}

	void ENetHost::SendAcknowledgements(ENetPeer* peer)
	{
		auto currentAcknowledgement = peer->m_acknowledgements.begin();
		while (currentAcknowledgement != peer->m_acknowledgements.end())
		{
			if (m_commandCount >= m_commands.size() || m_bufferCount >= m_buffers.size() || peer->m_mtu - m_packetSize < sizeof(ENetProtocolAcknowledge))
			{
				m_continueSending = true;
				break;
			}

			ENetPeer::Acknowledgement& acknowledgement = *currentAcknowledgement;

			ENetProtocol& command = m_commands[m_commandCount];
			NetBuffer& buffer = m_buffers[m_bufferCount];

			buffer.data = &command;
			buffer.dataLength = sizeof(ENetProtocolAcknowledge);

			m_packetSize += buffer.dataLength;

			UInt16 reliableSequenceNumber = HostToNet(acknowledgement.command.header.reliableSequenceNumber);

			command.header.command = ENetProtocolCommand_Acknowledge;
			command.header.channelID = acknowledgement.command.header.channelID;
			command.header.reliableSequenceNumber = reliableSequenceNumber;
			command.acknowledge.receivedReliableSequenceNumber = reliableSequenceNumber;
			command.acknowledge.receivedSentTime = HostToNet(acknowledgement.sentTime);

			if ((acknowledgement.command.header.command & ENetProtocolCommand_Mask) == ENetProtocolCommand_Disconnect)
				peer->DispatchState(ENetPeerState::Zombie);

			currentAcknowledgement = peer->m_acknowledgements.erase(currentAcknowledgement);

			++m_bufferCount;
			++m_commandCount;
		}
	}

	bool ENetHost::SendReliableOutgoingCommands(ENetPeer* peer)
	{
		bool canPing = true;
		bool windowExceeded = false;
		bool windowWrap = false;

		auto currentCommand = peer->m_outgoingReliableCommands.begin();
		while (currentCommand != peer->m_outgoingReliableCommands.end())
		{
			auto outgoingCommand = currentCommand;

			UInt16 reliableWindow = outgoingCommand->reliableSequenceNumber / ENetConstants::ENetPeer_ReliableWindowSize;
			ENetPeer::Channel* channel = (outgoingCommand->command.header.channelID < peer->m_channels.size()) ? &peer->m_channels[outgoingCommand->command.header.channelID] : nullptr;
			if (channel)
			{
				if (!windowWrap && outgoingCommand->sendAttempts < 1 && !(outgoingCommand->reliableSequenceNumber % ENetPeer_ReliableWindowSize) &&
					((channel->reliableWindows[(reliableWindow + ENetPeer_ReliableWindows - 1) % ENetPeer_ReliableWindows] >= ENetPeer_ReliableWindowSize) ||
					channel->usedReliableWindows & ((((1 << ENetPeer_ReliableWindows) - 1) << reliableWindow) |
					(((1 << ENetPeer_FreeReliableWindows) - 1) >> (ENetPeer_ReliableWindows - reliableWindow)))))
					windowWrap = true;

				if (windowWrap)
				{
					++currentCommand;
					continue;
				}
			}

			if (outgoingCommand->packet)
			{
				if (!windowExceeded)
				{
					UInt32 windowSize = (peer->m_packetThrottle * peer->m_windowSize) / ENetPeer_PacketThrottleScale;

					if (peer->m_reliableDataInTransit + outgoingCommand->fragmentLength > std::max(windowSize, peer->m_mtu))
						windowExceeded = true;
				}

				if (windowExceeded)
				{
					++currentCommand;
					continue;
				}
			}

			canPing = false;

			std::size_t commandSize = s_commandSizes[outgoingCommand->command.header.command & ENetProtocolCommand_Mask];
			if (m_commandCount >= m_commands.size() || m_bufferCount + 1 >= m_buffers.size() || peer->m_mtu - m_packetSize < commandSize ||
				(outgoingCommand->packet && UInt16(peer->m_mtu - m_packetSize) < UInt16(commandSize + outgoingCommand->fragmentLength)))
			{
				m_continueSending = true;
				break;
			}

			++currentCommand;

			if (channel && outgoingCommand->sendAttempts < 1)
			{
				channel->usedReliableWindows |= 1 << reliableWindow;
				++channel->reliableWindows[reliableWindow];
			}

			++outgoingCommand->sendAttempts;

			if (outgoingCommand->roundTripTimeout == 0)
			{
				outgoingCommand->roundTripTimeout = peer->m_roundTripTime + 4 * peer->m_roundTripTimeVariance;
				outgoingCommand->roundTripTimeoutLimit = peer->m_timeoutLimit * outgoingCommand->roundTripTimeout;
			}

			if (peer->m_sentReliableCommands.empty())
				peer->m_nextTimeout = m_serviceTime + outgoingCommand->roundTripTimeout;

			peer->m_sentReliableCommands.emplace_back(std::move(*outgoingCommand));
			peer->m_outgoingReliableCommands.erase(outgoingCommand);

			outgoingCommand = peer->m_sentReliableCommands.end();
			--outgoingCommand;

			outgoingCommand->sentTime = m_serviceTime;

			ENetProtocol& command = m_commands[m_commandCount];
			NetBuffer& buffer = m_buffers[m_bufferCount];

			buffer.data = &command;
			buffer.dataLength = commandSize;

			m_packetSize += buffer.dataLength;
			m_headerFlags |= ENetProtocolHeaderFlag_SentTime;

			command = outgoingCommand->command;

			if (outgoingCommand->packet)
			{
				++m_bufferCount;

				NetBuffer& packetBuffer = m_buffers[m_bufferCount];
				packetBuffer.data = outgoingCommand->packet->data.GetData() + Nz::NetPacket::HeaderSize + outgoingCommand->fragmentOffset;
				packetBuffer.dataLength = outgoingCommand->fragmentLength;

				m_packetSize += packetBuffer.dataLength;

				peer->m_reliableDataInTransit += outgoingCommand->fragmentLength;
			}

			++peer->m_packetsSent;
			++m_bufferCount;
			++m_commandCount;
		}

		return canPing;
	}

	int ENetHost::SendOutgoingCommands(ENetEvent* event, bool checkForTimeouts)
	{
		std::array<UInt8, sizeof(ENetProtocolHeader) + sizeof(UInt32)> headerData;
		ENetProtocolHeader* header = reinterpret_cast<ENetProtocolHeader*>(headerData.data());

		m_continueSending = true;

		while (m_continueSending)
		{
			m_continueSending = false;

			for (std::size_t peer = 0; peer < m_peerCount; ++peer)
			{
				ENetPeer* currentPeer = &m_peers[peer];
				if (currentPeer->GetState() == ENetPeerState::Disconnected || currentPeer->GetState() == ENetPeerState::Zombie)
					continue;

				m_headerFlags = 0;
				m_commandCount = 0;
				m_bufferCount = 1;
				m_packetSize = sizeof(ENetProtocolHeader);

				if (!currentPeer->m_acknowledgements.empty())
					SendAcknowledgements(currentPeer);

				if (checkForTimeouts && !currentPeer->m_sentReliableCommands.empty() && ENET_TIME_GREATER_EQUAL(m_serviceTime, currentPeer->m_nextTimeout) && currentPeer->CheckTimeouts(event))
				{
					if (event && event->type != ENetEventType::None)
						return 1;
					else
						continue;
				}

				if ((currentPeer->m_outgoingReliableCommands.empty() || SendReliableOutgoingCommands(currentPeer)) && currentPeer->m_sentReliableCommands.empty() &&
					ENET_TIME_DIFFERENCE(m_serviceTime, currentPeer->m_lastReceiveTime) >= currentPeer->m_pingInterval && currentPeer->m_mtu - m_packetSize >= sizeof(ENetProtocolPing))
				{
					currentPeer->Ping();
					SendReliableOutgoingCommands(currentPeer);
				}

				if (!currentPeer->m_outgoingUnreliableCommands.empty())
					SendUnreliableOutgoingCommands(currentPeer);

				if (m_commandCount == 0)
					continue;

				if (currentPeer->m_packetLossEpoch == 0)
					currentPeer->m_packetLossEpoch = m_serviceTime;
				else if (ENET_TIME_DIFFERENCE(m_serviceTime, currentPeer->m_packetLossEpoch) >= ENetPeer_PacketLossInterval && currentPeer->m_packetsSent > 0)
				{
					UInt32 packetLoss = currentPeer->m_packetsLost * ENetPeer_PacketLossScale / currentPeer->m_packetsSent;

					#ifdef ENET_DEBUG
					printf("peer %u: %f%%+-%f%% packet loss, %u+-%u ms round trip time, %f%% throttle, %u/%u outgoing, %u/%u incoming\n", currentPeer->incomingPeerID, currentPeer->packetLoss / (float) ENET_PEER_PACKET_LOSS_SCALE, currentPeer->packetLossVariance / (float) ENET_PEER_PACKET_LOSS_SCALE, currentPeer->roundTripTime, currentPeer->roundTripTimeVariance, currentPeer->packetThrottle / (float) ENET_PEER_PACKET_THROTTLE_SCALE, enet_list_size(&currentPeer->outgoingReliableCommands), enet_list_size(&currentPeer->outgoingUnreliableCommands), currentPeer->channels != NULL ? enet_list_size(&currentPeer->channels->incomingReliableCommands) : 0, currentPeer->channels != NULL ? enet_list_size(&currentPeer->channels->incomingUnreliableCommands) : 0);
					#endif

					currentPeer->m_packetLossVariance -= currentPeer->m_packetLossVariance / 4;

					if (packetLoss >= currentPeer->m_packetLoss)
					{
						currentPeer->m_packetLoss += (packetLoss - currentPeer->m_packetLoss) / 8;
						currentPeer->m_packetLossVariance += (packetLoss - currentPeer->m_packetLoss) / 4;
					}
					else
					{
						currentPeer->m_packetLoss -= (currentPeer->m_packetLoss - packetLoss) / 8;
						currentPeer->m_packetLossVariance += (currentPeer->m_packetLoss - packetLoss) / 4;
					}

					currentPeer->m_packetLossEpoch = m_serviceTime;
					currentPeer->m_packetsSent = 0;
					currentPeer->m_packetsLost = 0;
				}


				m_buffers[0].data = headerData.data();
				if (m_headerFlags & ENetProtocolHeaderFlag_SentTime)
				{
					header->sentTime = HostToNet(static_cast<UInt16>(m_serviceTime));

					m_buffers[0].dataLength = sizeof(ENetProtocolHeader);
				}
				else
					m_buffers[0].dataLength = NazaraOffsetOf(ENetProtocolHeader, sentTime);

				if (currentPeer->m_outgoingPeerID < ENetConstants::ENetProtocol_MaximumPeerId)
					m_headerFlags |= currentPeer->m_outgoingSessionID << ENetProtocolHeaderSessionShift;

				header->peerID = HostToNet(static_cast<UInt16>(currentPeer->m_outgoingPeerID | m_headerFlags));

				currentPeer->m_lastSendTime = m_serviceTime;

				std::size_t sentLength;
				if (!m_socket.SendMultiple(currentPeer->m_address, m_buffers.data(), m_bufferCount, &sentLength))
					return -1;

				currentPeer->RemoveSentUnreliableCommands();

				m_totalSentData += sentLength;
				m_totalSentPackets++;
			}
		}

		return 0;
	}

	void ENetHost::SendUnreliableOutgoingCommands(ENetPeer* peer)
	{
		auto currentCommand = peer->m_outgoingUnreliableCommands.begin();
		while (currentCommand != peer->m_outgoingUnreliableCommands.end())
		{
			auto outgoingCommand = currentCommand;

			std::size_t commandSize = s_commandSizes[outgoingCommand->command.header.command & ENetProtocolCommand_Mask];

			if (m_commandCount >= m_commands.size() || m_bufferCount + 1 >= m_buffers.size() || peer->m_mtu - m_packetSize < commandSize ||
			    (outgoingCommand->packet && peer->m_mtu - m_packetSize < commandSize + outgoingCommand->fragmentLength))
			{
				m_continueSending = true;
				break;
			}

			++currentCommand;

			if (outgoingCommand->packet && outgoingCommand->fragmentOffset == 0)
			{
				peer->m_packetThrottleCounter += ENetConstants::ENetPeer_PacketThrottleCounter;
				peer->m_packetThrottleCounter %= ENetConstants::ENetPeer_PacketThrottleScale;

				if (peer->m_packetThrottleCounter > peer->m_packetThrottle)
				{
					UInt16 reliableSequenceNumber = outgoingCommand->reliableSequenceNumber;
					UInt16 unreliableSequenceNumber = outgoingCommand->unreliableSequenceNumber;

					for (;;)
					{
						peer->m_outgoingUnreliableCommands.erase(outgoingCommand);

						if (currentCommand == peer->m_outgoingUnreliableCommands.end())
							break;

						outgoingCommand = currentCommand;
						if (outgoingCommand->reliableSequenceNumber != reliableSequenceNumber || outgoingCommand->unreliableSequenceNumber != unreliableSequenceNumber)
							break;

						++currentCommand;
					}

					continue;
				}
			}

			ENetProtocol& command = m_commands[m_commandCount];
			NetBuffer& buffer = m_buffers[m_bufferCount];

			buffer.data = &command;
			buffer.dataLength = commandSize;

			command = outgoingCommand->command;

			if (outgoingCommand->packet)
			{
				++m_bufferCount;

				NetBuffer& packetBuffer = m_buffers[m_bufferCount];
				packetBuffer.data = outgoingCommand->packet->data.GetData() + Nz::NetPacket::HeaderSize + outgoingCommand->fragmentOffset;
				packetBuffer.dataLength = outgoingCommand->fragmentLength;

				m_packetSize += packetBuffer.dataLength;

				peer->m_sentUnreliableCommands.emplace_back(std::move(*outgoingCommand));
			}

			peer->m_outgoingUnreliableCommands.erase(outgoingCommand);

			++m_bufferCount;
			++m_commandCount;
		}

		if (peer->GetState() == ENetPeerState::DisconnectLater && !peer->HasPendingCommands())
			peer->Disconnect(peer->m_eventData);
	}

	void ENetHost::ThrottleBandwidth()
	{
		UInt32 currentTime = GetElapsedMilliseconds();
		UInt32 elapsedTime = currentTime - m_bandwidthThrottleEpoch;

		if (elapsedTime < ENetConstants::ENetHost_BandwidthThrottleInterval)
			return;

		m_bandwidthThrottleEpoch = currentTime;

		if (m_connectedPeers == 0)
			return;

		UInt32 dataTotal = ~0;
		UInt32 bandwidth = ~0;

		if (m_outgoingBandwidth != 0)
		{
			bandwidth = (m_outgoingBandwidth * elapsedTime) / 1000;

			dataTotal = 0;
			for (ENetPeer& peer : m_peers)
			{
				if (peer.IsConnected())
					continue;

				dataTotal += peer.m_outgoingDataTotal;
			}
		}

		UInt32 peersRemaining = m_connectedPeers;
		UInt32 bandwidthLimit = ~0;
		UInt32 throttle = ~0;
		bool needsAdjustment = m_bandwidthLimitedPeers > 0;

		while (peersRemaining > 0 && needsAdjustment)
		{
			needsAdjustment = false;

			if (dataTotal <= bandwidth)
				throttle = ENetConstants::ENetPeer_PacketThrottleScale;
			else
				throttle = (bandwidth * ENetConstants::ENetPeer_PacketThrottleScale) / dataTotal;

			for (ENetPeer& peer : m_peers)
			{
				if (!peer.IsConnected() || peer.m_incomingBandwidth == 0 || peer.m_outgoingBandwidthThrottleEpoch == currentTime)
					continue;

				UInt32 peerBandwidth = (peer.m_incomingBandwidth * elapsedTime) / 1000;
				if ((throttle * peer.m_outgoingDataTotal) / ENetConstants::ENetPeer_PacketThrottleScale <= peerBandwidth)
					continue;

				peer.m_packetThrottleLimit = Clamp<UInt32>((peerBandwidth * ENetConstants::ENetPeer_PacketThrottleScale) / peer.m_outgoingDataTotal, 0, peer.m_packetThrottleLimit);
				peer.m_outgoingBandwidthThrottleEpoch = currentTime;

				peer.m_incomingDataTotal = 0;
				peer.m_outgoingDataTotal = 0;

				needsAdjustment = true;
				--peersRemaining;
				bandwidth -= peerBandwidth;
				dataTotal -= peerBandwidth;
			}
		}

		if (peersRemaining > 0)
		{
			if (dataTotal <= bandwidth)
				throttle = ENetConstants::ENetPeer_PacketThrottleScale;
			else
				throttle = (bandwidth * ENetConstants::ENetPeer_PacketThrottleScale) / dataTotal;

			for (ENetPeer& peer : m_peers)
			{
				if (!peer.IsConnected() || peer.m_outgoingBandwidthThrottleEpoch == currentTime)
					continue;

				peer.m_packetThrottleLimit = throttle;
				peer.m_packetThrottle = std::min(peer.m_packetThrottle, peer.m_packetThrottleLimit);

				peer.m_incomingDataTotal = 0;
				peer.m_outgoingDataTotal = 0;
			}
		}

		if (m_recalculateBandwidthLimits)
		{
			m_recalculateBandwidthLimits = false;

			peersRemaining = m_connectedPeers;
			bandwidth = m_incomingBandwidth;
			needsAdjustment = true;

			if (bandwidth == 0)
				bandwidthLimit = 0;
			else
			{
				while (peersRemaining > 0 && needsAdjustment)
				{
					needsAdjustment = false;
					bandwidthLimit = bandwidth / peersRemaining;

					for (ENetPeer& peer : m_peers)
					{
						if (!peer.IsConnected() || peer.m_incomingBandwidthThrottleEpoch == currentTime)
							continue;

						if (peer.m_outgoingBandwidth > 0 && peer.m_outgoingBandwidth >= bandwidthLimit)
							continue;

						peer.m_incomingBandwidthThrottleEpoch = currentTime;

						needsAdjustment = true;
						--peersRemaining;
						bandwidth -= peer.m_outgoingBandwidth;
					}
				}
			}

			for (ENetPeer& peer : m_peers)
			{
				if (!peer.IsConnected())
					continue;

				ENetProtocol command(ENetProtocolCommand_BandwidthLimit | ENetProtocolFlag_Acknowledge, 0xFF);
				command.bandwidthLimit.outgoingBandwidth = HostToNet(m_outgoingBandwidth);

				if (peer.m_incomingBandwidthThrottleEpoch == currentTime)
					command.bandwidthLimit.incomingBandwidth = HostToNet(peer.m_outgoingBandwidth);
				else
					command.bandwidthLimit.incomingBandwidth = HostToNet(bandwidthLimit);

				peer.QueueOutgoingCommand(command);
			}
		}
	}

	std::size_t ENetHost::GetCommandSize(UInt8 commandNumber)
	{
		return s_commandSizes[commandNumber & ENetProtocolCommand_Mask];
	}

	bool ENetHost::Initialize()
	{
		std::random_device device;
		s_randomGenerator.seed(device());
		s_randomGenerator64.seed(device());

		return true;
	}

	void ENetHost::Uninitialize()
	{
	}

	std::mt19937 ENetHost::s_randomGenerator;
	std::mt19937_64 ENetHost::s_randomGenerator64;
}