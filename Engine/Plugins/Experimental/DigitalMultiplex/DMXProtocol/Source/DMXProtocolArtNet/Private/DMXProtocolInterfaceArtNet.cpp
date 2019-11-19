#include "DMXProtocolInterfaceArtNet.h"
#include "Interfaces/IDMXProtocol.h"

FDMXProtocolInterfaceArtNet::FDMXProtocolInterfaceArtNet(IDMXProtocol* InDMXProtocol)
	: DMXProtocol(InDMXProtocol)
{
}

IDMXProtocol* FDMXProtocolInterfaceArtNet::GetProtocol() const
{
	return DMXProtocol;
}