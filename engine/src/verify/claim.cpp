#include "claim.h"

namespace verify
{

const char *claimTypeName(ClaimType t)
{
	switch (t) {
	case ClaimType::CapabilityExists:
		return "CapabilityExists";
	case ClaimType::ContractHolds:
		return "ContractHolds";
	case ClaimType::ArchitectureFollows:
		return "ArchitectureFollows";
	case ClaimType::FunctionImplements:
		return "FunctionImplements";
	}
	return "UnknownClaimType";
}

const char *verdictName(Verdict v)
{
	switch (v) {
	case Verdict::Supported:
		return "Supported";
	case Verdict::Contradicted:
		return "Contradicted";
	case Verdict::Unknown:
		return "Unknown";
	}
	return "UnknownVerdict";
}

} // namespace verify
