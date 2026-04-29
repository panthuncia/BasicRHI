#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <deque>
#include <charconv>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rhi.h"
#include "rhi_debug_internal.h"

#if BASICRHI_ENABLE_RESHAPE
#include <Message/MessageStream.h>
#include <Bridge/Log/LogSeverity.h>
#include <Schemas/Diagnostic.h>
#include <Schemas/Feature.h>
#include <Schemas/Instrumentation.h>
#include <Schemas/Object.h>
#include <Schemas/PipelineMetadata.h>
struct MessageStream;
#endif

namespace rhi {
	struct ReShapeBackendRuntimeAdapter {
		virtual ~ReShapeBackendRuntimeAdapter() = default;

	#if BASICRHI_ENABLE_RESHAPE
		virtual Result CommitMessages(MessageStream& stream) noexcept = 0;
		virtual bool TryDequeueCapturedStream(MessageStream& stream) noexcept = 0;
		virtual uint32_t QueryOutputStreamCount() noexcept = 0;
		virtual uint32_t ConsumeOutputStreams(MessageStream* streams, uint32_t capacity) noexcept = 0;
		virtual void FreeOutputStream(MessageStream& stream) noexcept = 0;
	#endif
	};

	struct ReShapePendingInstrumentationPipelineIssue {
		DebugInstrumentationDiagnosticSeverity severity = DebugInstrumentationDiagnosticSeverity::Info;
		uint64_t pipelineUid = 0;
		std::string message;
		std::vector<uint32_t> rollingExecutionUids;
	};

	struct ReShapePendingInstrumentationShaderIssue {
		DebugInstrumentationDiagnosticSeverity severity = DebugInstrumentationDiagnosticSeverity::Info;
		uint64_t shaderUid = 0;
		uint64_t pipelineUid = 0;
		uint64_t sguid = 0;
		std::string message;
		std::vector<uint32_t> rollingExecutionUids;
	};

	enum class ReShapeInstrumentationExecutionKind : uint8_t {
		Unknown,
		DescriptorMismatch,
		ResourceIndexOutOfBounds,
	};

	struct ReShapeInstrumentationTracebackDetail {
		bool valid = false;
		uint32_t executionFlag = 0;
		uint32_t rollingExecutionUid = 0;
		uint32_t pipelineUid = 0;
		std::array<uint32_t, 5> markerHashes32{};
		uint32_t queueUid = 0;
		std::array<uint32_t, 3> kernelLaunch{};
		std::array<uint32_t, 3> thread{};
	};

	struct ReShapeInstrumentationDescriptorMismatchDetail {
		bool hasDetail = false;
		uint32_t token = 0;
		uint32_t compileType = 0;
		uint32_t runtimeType = 0;
		bool isUndefined = false;
		bool isOutOfBounds = false;
		bool isTableNotBound = false;
	};

	struct ReShapeInstrumentationResourceBoundsDetail {
		bool hasDetail = false;
		uint32_t token = 0;
		std::array<uint32_t, 3> coordinate{};
		bool isTexture = false;
		bool isWrite = false;
	};

	struct ReShapeInstrumentationExecutionDetail {
		uint64_t detailId = 0;
		DebugInstrumentationDiagnosticSeverity severity = DebugInstrumentationDiagnosticSeverity::Info;
		ReShapeInstrumentationExecutionKind kind = ReShapeInstrumentationExecutionKind::Unknown;
		uint64_t shaderUid = 0;
		uint64_t pipelineUid = 0;
		uint64_t sguid = 0;
		std::string message;
		ReShapeInstrumentationTracebackDetail traceback;
		ReShapeInstrumentationDescriptorMismatchDetail descriptorMismatch;
		ReShapeInstrumentationResourceBoundsDetail resourceBounds;
	};

	struct ReShapeInstrumentationPipelineMetadata {
		DebugInstrumentationPipelineKind kind = DebugInstrumentationPipelineKind::Unknown;
		bool active = false;
		bool instrumented = false;
		bool explicitlyInstrumented = false;
		uint64_t explicitFeatureMask = 0;
		std::string label;
		std::string lastPassName;
	};

	struct ReShapeInstrumentationPipelineUsage {
		uint64_t pipelineUid = 0;
		std::string techniquePath;
		std::string passName;
	};

	struct ReShapeDebugInstrumentationSessionCore {
		DebugInstrumentationCapabilities capabilities{};
		DebugInstrumentationState state{};
		std::vector<DebugInstrumentationFeature> features;
		std::deque<DebugInstrumentationDiagnostic> diagnostics;
		std::vector<DebugInstrumentationIssue> issues;
		std::vector<ReShapePendingInstrumentationPipelineIssue> pendingPipelineIssues;
		std::vector<ReShapePendingInstrumentationShaderIssue> pendingShaderIssues;
		std::unordered_map<uint64_t, std::string> pipelineNames;
		std::unordered_map<uint64_t, ReShapeInstrumentationPipelineMetadata> pipelines;
		std::vector<uint64_t> pipelineOrder;
		std::vector<ReShapeInstrumentationPipelineUsage> pipelineUsages;
		std::unordered_map<std::string, size_t> pipelineUsageIndexByKey;
		std::unordered_map<uint32_t, std::string> executionStacks;
		std::deque<ReShapeInstrumentationExecutionDetail> executionDetails;
		std::unordered_map<std::string, std::deque<ReShapeInstrumentationExecutionDetail>> archivedExecutionDetailsByKey;
		std::unordered_map<uint64_t, ReShapeInstrumentationExecutionDetail> retainedExecutionDetails;
		std::unordered_set<uint64_t> requestedPipelineNames;
		std::unordered_set<uint64_t> pendingPipelineNameRequests;
		std::unordered_set<uint64_t> pendingPipelineStatusRequests;
		uint32_t knownShaderCount = 0;
		uint32_t knownPipelineCount = 0;
		bool pipelineInventoryRefreshRequested = false;
		std::unordered_set<uint64_t> pendingShaderCodeRequests;
		std::unordered_set<uint64_t> requestedShaderSourceMappings;
		std::unordered_set<uint64_t> pendingShaderSourceMappingRequests;
		std::unordered_set<uint64_t> suspiciousShaderFileResolutionTelemetrySguids;
		uint64_t nextExecutionDetailId = 1;
		bool issuesDirty = false;
		bool pollInProgress = false;
		bool defaultDescriptorMaskApplied = false;
		bool explicitGlobalFeatureMaskConfigured = false;
		uint32_t featureQueryAttempts = 0;
		bool featureQueryCompleted = false;
		ReShapeBackendRuntimeAdapter* runtime = nullptr;
		std::mutex mutex;
	};

	namespace reshape {
		template <typename SessionT>
		inline ReShapeInstrumentationPipelineMetadata& ReShapeGetOrCreatePipelineMetadataUnlocked(
			SessionT& session,
			uint64_t pipelineUid);

		inline DebugInstrumentationPipelineKind ReShapeMapPipelineKind(uint32_t type) noexcept {
			switch (type) {
			case 2u:
				return DebugInstrumentationPipelineKind::Graphics;
			case 4u:
				return DebugInstrumentationPipelineKind::Compute;
			case 8u:
				return DebugInstrumentationPipelineKind::StateObject;
			default:
				return DebugInstrumentationPipelineKind::Unknown;
			}
		}

	#if BASICRHI_ENABLE_RESHAPE
		template <typename SessionT, typename DiagnosticFn, typename PollFn>
		inline Result ReShapeCommitMessages(
			SessionT& session,
			MessageStream& stream,
			DiagnosticFn&& appendDiagnostic,
			PollFn&& pollNow) {
			if (!session.runtime) {
				return Result::Unsupported;
			}

			const Result commitResult = session.runtime->CommitMessages(stream);
			if (commitResult == Result::Unsupported) {
				return Result::Unsupported;
			}
			if (commitResult != Result::Ok) {
				appendDiagnostic(
					DebugInstrumentationDiagnosticSeverity::Error,
					"Bridge output storage is unavailable during GPU-Reshape message commit.");
				return Result::Failed;
			}

			bool shouldPollImmediately = false;
			{
				std::lock_guard guard(session.mutex);
				shouldPollImmediately = !session.pollInProgress;
			}
			if (shouldPollImmediately) {
				pollNow();
			}

			return Result::Ok;
		}

		inline DebugInstrumentationDiagnosticSeverity ReShapeMapDiagnosticSeverity(uint32_t severity) noexcept {
			switch (static_cast<LogSeverity>(severity)) {
			case LogSeverity::Info:
				return DebugInstrumentationDiagnosticSeverity::Info;
			case LogSeverity::Warning:
				return DebugInstrumentationDiagnosticSeverity::Warning;
			case LogSeverity::Error:
			default:
				return DebugInstrumentationDiagnosticSeverity::Error;
			}
		}

		template <typename SessionT>
		inline uint64_t ReShapeFindFeatureBitByNameUnlocked(const SessionT& session, const char* featureName) noexcept {
			if (!featureName || !featureName[0]) {
				return 0;
			}

			for (const DebugInstrumentationFeature& feature : session.features) {
				if (std::strcmp(feature.name, featureName) == 0) {
					return feature.featureBit;
				}
			}

			return 0;
		}

		inline void ReShapeBuildDefaultInstrumentationSpecialization(MessageStream& out) noexcept {
			auto* config = MessageStreamView<>(out).Add<SetInstrumentationConfigMessage>();
			config->validationCoverage = 0;
			config->safeGuard = 0;
			config->detail = 1;
			config->traceback = 1;
		}

		template <typename TMessage>
		inline uint32_t ReShapeGetChunkMask(const TMessage& message) noexcept {
			return *reinterpret_cast<const uint32_t*>(&message) >> (32u - static_cast<uint32_t>(TMessage::Chunk::Count));
		}

		template <typename SessionT>
		inline std::vector<DebugInstrumentationFeature> ReShapeCollectFeatureDescriptors(const SessionT&, const FeatureListMessage& message) {
			MessageStream descriptorStream;
			message.descriptors.Transfer(descriptorStream);

			std::vector<DebugInstrumentationFeature> features;
			for (auto it = MessageStreamView(descriptorStream).GetIterator(); it; ++it) {
				if (!it.Is(FeatureDescriptorMessage::kID)) {
					continue;
				}

				auto* descriptor = it.Get<FeatureDescriptorMessage>();
				const std::string featureName{ descriptor->name.View() };
				const std::string featureDescription{ descriptor->description.View() };
				DebugInstrumentationFeature feature{};
				feature.featureBit = descriptor->featureBit;
				std::snprintf(feature.name, sizeof(feature.name), "%s", featureName.c_str());
				std::snprintf(feature.description, sizeof(feature.description), "%s", featureDescription.c_str());
				features.push_back(feature);
			}

			return features;
		}

		template <typename SessionT>
		inline bool ReShapeShouldRefreshFeatureListUnlocked(const SessionT& session) noexcept {
			return !session.state.active
				? false
				: (!session.featureQueryCompleted || (session.features.empty() && session.featureQueryAttempts < 3));
		}

		template <typename SessionT, typename CommitFn, typename DiagnosticFn>
		inline Result ReShapeRefreshFeatures(SessionT& session, CommitFn&& commitMessages, DiagnosticFn&& appendDiagnostic) {
			uint32_t attempt = 0;
			{
				std::lock_guard guard(session.mutex);
				attempt = ++session.featureQueryAttempts;
			}

			char infoMessage[128] = {};
			std::snprintf(
				infoMessage,
				sizeof(infoMessage),
				"Requesting backend feature list (attempt %u).",
				attempt);
			appendDiagnostic(DebugInstrumentationDiagnosticSeverity::Info, infoMessage);

			MessageStream stream;
			auto* message = MessageStreamView<>(stream).Add<GetFeaturesMessage>();
			message->featureBitSet = ~0ull;
			const Result result = commitMessages(stream);
			if (result != Result::Ok) {
				appendDiagnostic(DebugInstrumentationDiagnosticSeverity::Warning, "Backend feature list request did not complete cleanly.");
			}
			return result;
		}

		template <typename SessionT>
		inline void ReShapeApplyPipelineNameMessageUnlocked(SessionT& session, uint64_t pipelineUid, uint32_t type, std::string_view name) {
			auto& metadata = ReShapeGetOrCreatePipelineMetadataUnlocked(session, pipelineUid);
			metadata.kind = ReShapeMapPipelineKind(type);
			metadata.label.assign(name.data(), name.size());
			session.pipelineNames[pipelineUid] = metadata.label;
			session.issuesDirty = true;
		}

		template <typename SessionT>
		inline void ReShapeApplyObjectStatesMessageUnlocked(SessionT& session, const ObjectStatesMessage& message) noexcept {
			session.knownShaderCount = message.shaderCount;
			session.knownPipelineCount = message.pipelineCount;
			session.pipelineInventoryRefreshRequested = false;
		}

		template <typename SessionT>
		inline void ReShapeApplyPipelineUidRangeMessageUnlocked(SessionT& session, const PipelineUIDRangeMessage& message) {
			for (uint64_t index = 0; index < message.pipelineUID.count; ++index) {
				const uint64_t pipelineUid = message.pipelineUID[index];
				(void)ReShapeGetOrCreatePipelineMetadataUnlocked(session, pipelineUid);
				if (session.pipelineNames.find(pipelineUid) == session.pipelineNames.end()) {
					session.pendingPipelineNameRequests.insert(pipelineUid);
				}
				session.pendingPipelineStatusRequests.insert(pipelineUid);
			}
		}

		template <typename SessionT>
		inline void ReShapeApplyPipelineStatusCollectionMessageUnlocked(SessionT& session, const PipelineStatusCollectionMessage& message) {
			MessageStream statusStream;
			message.status.Transfer(statusStream);
			ConstMessageStreamView<PipelineStatusMessage> statusView(statusStream);
			for (auto statusIt = statusView.GetIterator(); statusIt; ++statusIt) {
				auto& metadata = ReShapeGetOrCreatePipelineMetadataUnlocked(session, statusIt->pipelineUID);
				metadata.active = statusIt->active != 0;
				metadata.instrumented = statusIt->instrumented != 0;
			}
		}

		template <typename SessionT>
		inline void ReShapeApplyGlobalInstrumentationMessageUnlocked(SessionT& session, const SetGlobalInstrumentationMessage& message) noexcept {
			session.state.globalFeatureMask = message.featureBitSet;
		}

		template <typename DiagnosticFn>
		inline void ReShapeAppendInstrumentationDiagnosticSummary(const InstrumentationDiagnosticMessage& message, DiagnosticFn&& appendDiagnostic) {
			char summary[160] = {};
			if (message.failedShaders != 0 || message.failedPipelines != 0) {
				std::snprintf(
					summary,
					sizeof(summary),
					"Instrumentation failed for %u shader(s) and %u pipeline(s).",
					message.failedShaders,
					message.failedPipelines);
				appendDiagnostic(DebugInstrumentationDiagnosticSeverity::Error, summary);
			}

			std::snprintf(
				summary,
				sizeof(summary),
				"Instrumented %u shader(s) (%u ms) and %u pipeline(s) (%u ms), total %u ms.",
				message.passedShaders,
				message.millisecondsShaders,
				message.passedPipelines,
				message.millisecondsPipelines,
				message.millisecondsTotal);
			appendDiagnostic(DebugInstrumentationDiagnosticSeverity::Info, summary);

			MessageStream diagnosticStream;
			message.messages.Transfer(diagnosticStream);
			ConstMessageStreamView<CompilationDiagnosticMessage> diagnosticsView(diagnosticStream);
			for (auto diagIt = diagnosticsView.GetIterator(); diagIt; ++diagIt) {
				appendDiagnostic(DebugInstrumentationDiagnosticSeverity::Error, diagIt->content.View().data());
			}
		}

		template <typename SessionT>
		inline void ReShapeBuildCommonOrderedRequestsUnlocked(
			SessionT& session,
			MessageStream& requestStream,
			size_t maxPipelineNamesPerRound,
			size_t maxPipelineStatusesPerRound,
			uint32_t maxPipelineUidRangePerRound) {
			if (session.pipelineInventoryRefreshRequested) {
				auto* message = MessageStreamView<>(requestStream).Add<GetObjectStatesMessage>();
				message->ignore = 0;
				session.pipelineInventoryRefreshRequested = false;
			}

			const uint32_t knownPipelineCount = session.knownPipelineCount;
			const uint32_t enumeratedPipelineCount = static_cast<uint32_t>(session.pipelineOrder.size());
			if (knownPipelineCount > enumeratedPipelineCount) {
				auto* message = MessageStreamView<>(requestStream).Add<GetPipelineUIDRangeMessage>();
				message->start = enumeratedPipelineCount;
				message->limit = (std::min)(knownPipelineCount - enumeratedPipelineCount, maxPipelineUidRangePerRound);
			}

			size_t pipelineNameCount = 0;
			for (auto it = session.pendingPipelineNameRequests.begin();
				it != session.pendingPipelineNameRequests.end() && pipelineNameCount < maxPipelineNamesPerRound;) {
				const uint64_t pipelineUid = *it;
				auto* message = MessageStreamView<>(requestStream).Add<GetPipelineNameMessage>();
				message->pipelineUID = pipelineUid;
				session.requestedPipelineNames.insert(pipelineUid);
				it = session.pendingPipelineNameRequests.erase(it);
				++pipelineNameCount;
			}

			if (!session.pendingPipelineStatusRequests.empty()) {
				std::vector<uint64_t> pipelineStatusBatch;
				pipelineStatusBatch.reserve((std::min)(session.pendingPipelineStatusRequests.size(), maxPipelineStatusesPerRound));
				for (auto it = session.pendingPipelineStatusRequests.begin();
					it != session.pendingPipelineStatusRequests.end() && pipelineStatusBatch.size() < maxPipelineStatusesPerRound;) {
					pipelineStatusBatch.push_back(*it);
					it = session.pendingPipelineStatusRequests.erase(it);
				}
				auto* message = MessageStreamView<>(requestStream).Add<GetPipelineStatusMessage>(GetPipelineStatusMessage::AllocationInfo{
					.pipelineUIDsCount = pipelineStatusBatch.size()
				});
				size_t pipelineUidIndex = 0;
				for (uint64_t pipelineUid : pipelineStatusBatch) {
					message->pipelineUIDs[pipelineUidIndex++] = pipelineUid;
				}
			}
		}
	#endif

		inline bool ReShapeTryParseIssueUid(const char* message, const char* prefix, uint64_t& uid) noexcept {
			if (!message || !prefix) {
				return false;
			}

			const size_t prefixLength = std::strlen(prefix);
			if (std::strncmp(message, prefix, prefixLength) != 0) {
				return false;
			}

			const char* first = message + prefixLength;
			const char* last = first;
			while (*last >= '0' && *last <= '9') {
				++last;
			}

			if (first == last) {
				return false;
			}

			uid = 0;
			const auto result = std::from_chars(first, last, uid);
			return result.ec == std::errc{};
		}

		template <typename QueuePipelineIssueT, typename QueueShaderIssueT>
		inline void ReShapeMarkIssueFromMessage(
			DebugInstrumentationDiagnosticSeverity severity,
			const char* message,
			QueuePipelineIssueT&& queuePipelineIssue,
			QueueShaderIssueT&& queueShaderIssue) {
			if (severity == DebugInstrumentationDiagnosticSeverity::Info || !message || !message[0]) {
				return;
			}

			uint64_t uid = 0;
			if (ReShapeTryParseIssueUid(message, "Pipeline ", uid)) {
				queuePipelineIssue(uid, message);
				return;
			}

			if (ReShapeTryParseIssueUid(message, "Shader ", uid)) {
				queueShaderIssue(uid, message);
			}
		}

		inline std::string ReShapeFormatPipelineLabel(uint64_t pipelineUid, const std::unordered_map<uint64_t, std::string>& pipelineNames) {
			if (pipelineUid == 0) {
				return "Unknown pipeline";
			}

			const auto pipelineNameIt = pipelineNames.find(pipelineUid);
			if (pipelineNameIt != pipelineNames.end() && !pipelineNameIt->second.empty()) {
				return pipelineNameIt->second;
			}

			char label[64] = {};
			std::snprintf(label, sizeof(label), "Pipeline %llu", static_cast<unsigned long long>(pipelineUid));
			return label;
		}

		inline std::string ReShapeMakePipelineUsageKey(uint64_t pipelineUid, std::string_view techniquePath) {
			std::string key = std::to_string(pipelineUid);
			key.push_back('|');
			key.append(techniquePath.data(), techniquePath.size());
			return key;
		}

		template <typename SessionT>
		inline ReShapeInstrumentationPipelineMetadata& ReShapeGetOrCreatePipelineMetadataUnlocked(
			SessionT& session,
			uint64_t pipelineUid) {
			auto [it, inserted] = session.pipelines.try_emplace(pipelineUid);
			if (inserted) {
				session.pipelineOrder.push_back(pipelineUid);
			}
			return it->second;
		}

		template <typename SessionT>
		inline void ReShapeObservePipelineUsageUnlocked(
			SessionT& session,
			uint64_t pipelineUid,
			DebugInstrumentationPipelineKind kind,
			const char* label,
			const char* passName,
			const char* techniquePath) {
			if (pipelineUid == 0 || pipelineUid == ~0ull) {
				return;
			}

			auto& metadata = ReShapeGetOrCreatePipelineMetadataUnlocked(session, pipelineUid);
			if (metadata.kind == DebugInstrumentationPipelineKind::Unknown && kind != DebugInstrumentationPipelineKind::Unknown) {
				metadata.kind = kind;
			}
			if (label && *label) {
				metadata.label = label;
				session.pipelineNames[pipelineUid] = label;
			}
			if (passName && *passName) {
				metadata.lastPassName = passName;
			}
			if (techniquePath && *techniquePath) {
				const std::string key = ReShapeMakePipelineUsageKey(pipelineUid, techniquePath);
				auto usageIt = session.pipelineUsageIndexByKey.find(key);
				if (usageIt == session.pipelineUsageIndexByKey.end()) {
					session.pipelineUsageIndexByKey.emplace(key, session.pipelineUsages.size());
					session.pipelineUsages.push_back(ReShapeInstrumentationPipelineUsage{
						.pipelineUid = pipelineUid,
						.techniquePath = techniquePath,
						.passName = passName ? passName : ""
					});
				} else if (passName && *passName) {
					session.pipelineUsages[usageIt->second].passName = passName;
				}
			}
		}

		inline std::string ReShapeNormalizeIssueMessageForDeduplication(std::string_view input) {
			std::string normalized(input);
			constexpr std::string_view kExecutionToken = "execution ";
			size_t searchOffset = 0;
			while ((searchOffset = normalized.find(kExecutionToken, searchOffset)) != std::string::npos) {
				size_t digitOffset = searchOffset + kExecutionToken.size();
				size_t digitEnd = digitOffset;
				while (digitEnd < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[digitEnd])) != 0) {
					++digitEnd;
				}
				if (digitEnd > digitOffset) {
					normalized.replace(digitOffset, digitEnd - digitOffset, "#");
					searchOffset = digitOffset + 1;
				} else {
					searchOffset = digitOffset;
				}
			}
			return normalized;
		}

		inline void ReShapeAppendRollingExecutionUid(std::vector<uint32_t>& rollingExecutionUids, uint32_t rollingExecutionUid) {
			if (!rollingExecutionUid) {
				return;
			}

			if (std::find(rollingExecutionUids.begin(), rollingExecutionUids.end(), rollingExecutionUid) == rollingExecutionUids.end()) {
				rollingExecutionUids.push_back(rollingExecutionUid);
			}
		}

		inline std::string ReShapeBuildExecutionDetailRetentionKey(const ReShapeInstrumentationExecutionDetail& detail) {
			std::string key;
			key.reserve(256);
			key += std::to_string(static_cast<uint32_t>(detail.severity));
			key += '|';
			key += std::to_string(static_cast<uint32_t>(detail.kind));
			key += '|';
			key += std::to_string(detail.shaderUid);
			key += '|';
			key += std::to_string(detail.pipelineUid);
			key += '|';
			key += std::to_string(detail.sguid);
			key += '|';
			key += ReShapeNormalizeIssueMessageForDeduplication(detail.message);
			return key;
		}

		inline std::string ReShapeBuildIssueDedupKey(const DebugInstrumentationIssue& issue) {
			std::string key;
			key.reserve(512);
			key += std::to_string(static_cast<uint32_t>(issue.severity));
			key += '|';
			key += std::to_string(static_cast<uint32_t>(issue.type));
			key += '|';
			key += std::to_string(issue.objectUid);
			key += '|';
			key += issue.label;
			key += '|';
			key += issue.path;
			key += '|';
			key += ReShapeNormalizeIssueMessageForDeduplication(issue.message);
			return key;
		}

		template <typename IssueContainerT>
		inline void ReShapeAppendUniqueIssue(IssueContainerT& issues, std::unordered_set<std::string>& keys, const DebugInstrumentationIssue& issue) {
			const std::string key = ReShapeBuildIssueDedupKey(issue);
			if (keys.insert(key).second) {
				issues.push_back(issue);
			}
		}

		template <typename SessionT>
		inline void ReShapeAppendExecutionDetailUnlocked(SessionT& session, ReShapeInstrumentationExecutionDetail detail) {
			constexpr size_t kMaxExecutionDetails = 2048;
			constexpr size_t kMaxArchivedExecutionDetailsPerIssue = 16;
			if (!detail.detailId) {
				detail.detailId = session.nextExecutionDetailId++;
			}

			const std::string retentionKey = ReShapeBuildExecutionDetailRetentionKey(detail);
			auto& archivedDetails = session.archivedExecutionDetailsByKey[retentionKey];
			if (archivedDetails.size() >= kMaxArchivedExecutionDetailsPerIssue) {
				archivedDetails.pop_front();
			}
			archivedDetails.push_back(detail);

			if (session.executionDetails.size() >= kMaxExecutionDetails) {
				session.executionDetails.pop_front();
			}
			session.executionDetails.push_back(std::move(detail));
		}

		template <typename SessionT>
		inline bool ReShapeRetainExecutionDetailUnlocked(SessionT& session, uint64_t detailId) {
			if (!detailId) {
				return false;
			}

			if (session.retainedExecutionDetails.contains(detailId)) {
				return true;
			}

			auto retainIfMatch = [&](const ReShapeInstrumentationExecutionDetail& detail) {
				if (detail.detailId != detailId) {
					return false;
				}
				session.retainedExecutionDetails.emplace(detailId, detail);
				return true;
			};

			for (auto it = session.executionDetails.rbegin(); it != session.executionDetails.rend(); ++it) {
				if (retainIfMatch(*it)) {
					return true;
				}
			}

			for (const auto& [key, details] : session.archivedExecutionDetailsByKey) {
				(void)key;
				for (auto it = details.rbegin(); it != details.rend(); ++it) {
					if (retainIfMatch(*it)) {
						return true;
					}
				}
			}

			return false;
		}

		template <typename SessionT, typename IssueMatcherT, typename SnapshotBuilderT>
		inline std::vector<debug::InstrumentationExecutionDetailSnapshot> ReShapeCollectExecutionDetailSnapshotsUnlocked(
			const SessionT& session,
			const DebugInstrumentationIssue& issue,
			IssueMatcherT&& issueMatchesExecutionDetail,
			SnapshotBuilderT&& buildExecutionDetailSnapshot) {
			std::vector<debug::InstrumentationExecutionDetailSnapshot> snapshots;
			std::unordered_set<uint64_t> seenDetailIds;

			auto tryAppend = [&](const ReShapeInstrumentationExecutionDetail& detail) {
				if (!issueMatchesExecutionDetail(detail)) {
					return;
				}

				if (!seenDetailIds.insert(detail.detailId).second) {
					return;
				}

				snapshots.push_back(buildExecutionDetailSnapshot(detail));
			};

			for (const auto& [detailId, detail] : session.retainedExecutionDetails) {
				(void)detailId;
				tryAppend(detail);
			}

			for (const auto& [key, details] : session.archivedExecutionDetailsByKey) {
				(void)key;
				for (const ReShapeInstrumentationExecutionDetail& detail : details) {
					tryAppend(detail);
				}
			}

			for (const ReShapeInstrumentationExecutionDetail& detail : session.executionDetails) {
				tryAppend(detail);
			}

			std::sort(
				snapshots.begin(),
				snapshots.end(),
				[](const debug::InstrumentationExecutionDetailSnapshot& lhs, const debug::InstrumentationExecutionDetailSnapshot& rhs) {
					return lhs.detailId > rhs.detailId;
				});

			return snapshots;
		}

		template <typename SessionT>
		inline bool ReShapeHasPendingPipelineIssueUnlocked(
			const SessionT& session,
			DebugInstrumentationDiagnosticSeverity severity,
			uint64_t pipelineUid,
			const char* message) {
			const std::string normalizedMessage = ReShapeNormalizeIssueMessageForDeduplication(message ? message : "");
			for (const ReShapePendingInstrumentationPipelineIssue& issue : session.pendingPipelineIssues) {
				if (issue.severity == severity
					&& issue.pipelineUid == pipelineUid
					&& ReShapeNormalizeIssueMessageForDeduplication(issue.message) == normalizedMessage) {
					return true;
				}
			}
			return false;
		}

		template <typename SessionT>
		inline bool ReShapeHasPendingShaderIssueUnlocked(
			const SessionT& session,
			DebugInstrumentationDiagnosticSeverity severity,
			uint64_t shaderUid,
			uint64_t pipelineUid,
			uint64_t sguid,
			const char* message) {
			const std::string normalizedMessage = ReShapeNormalizeIssueMessageForDeduplication(message ? message : "");
			for (const ReShapePendingInstrumentationShaderIssue& issue : session.pendingShaderIssues) {
				if (issue.severity == severity
					&& issue.shaderUid == shaderUid
					&& issue.pipelineUid == pipelineUid
					&& issue.sguid == sguid
					&& ReShapeNormalizeIssueMessageForDeduplication(issue.message) == normalizedMessage) {
					return true;
				}
			}
			return false;
		}

		template <typename SessionT>
		inline void ReShapeQueuePendingPipelineIssueUnlocked(
			SessionT& session,
			DebugInstrumentationDiagnosticSeverity severity,
			uint64_t pipelineUid,
			const char* message,
			uint32_t rollingExecutionUid = 0) {
			if (!pipelineUid) {
				return;
			}

			if (!ReShapeHasPendingPipelineIssueUnlocked(session, severity, pipelineUid, message)) {
				session.pendingPipelineIssues.push_back(ReShapePendingInstrumentationPipelineIssue{
					.severity = severity,
					.pipelineUid = pipelineUid,
					.message = message ? message : ""
				});
				ReShapeAppendRollingExecutionUid(session.pendingPipelineIssues.back().rollingExecutionUids, rollingExecutionUid);
				session.issuesDirty = true;
			} else {
				const std::string normalizedMessage = ReShapeNormalizeIssueMessageForDeduplication(message ? message : "");
				for (ReShapePendingInstrumentationPipelineIssue& issue : session.pendingPipelineIssues) {
					if (issue.severity == severity
						&& issue.pipelineUid == pipelineUid
						&& ReShapeNormalizeIssueMessageForDeduplication(issue.message) == normalizedMessage) {
						ReShapeAppendRollingExecutionUid(issue.rollingExecutionUids, rollingExecutionUid);
						break;
					}
				}
			}

			if (!session.pipelineNames.contains(pipelineUid) && !session.requestedPipelineNames.contains(pipelineUid)) {
				session.pendingPipelineNameRequests.insert(pipelineUid);
			}
		}

		template <typename SessionT>
		inline void ReShapeQueuePendingShaderIssueUnlocked(
			SessionT& session,
			DebugInstrumentationDiagnosticSeverity severity,
			uint64_t shaderUid,
			uint64_t pipelineUid,
			uint64_t sguid,
			const char* message,
			uint32_t rollingExecutionUid = 0) {
			if (!shaderUid && !sguid) {
				return;
			}

			if (!ReShapeHasPendingShaderIssueUnlocked(session, severity, shaderUid, pipelineUid, sguid, message)) {
				session.pendingShaderIssues.push_back(ReShapePendingInstrumentationShaderIssue{
					.severity = severity,
					.shaderUid = shaderUid,
					.pipelineUid = pipelineUid,
					.sguid = sguid,
					.message = message ? message : ""
				});
				ReShapeAppendRollingExecutionUid(session.pendingShaderIssues.back().rollingExecutionUids, rollingExecutionUid);
				session.issuesDirty = true;
			} else {
				const std::string normalizedMessage = ReShapeNormalizeIssueMessageForDeduplication(message ? message : "");
				for (ReShapePendingInstrumentationShaderIssue& issue : session.pendingShaderIssues) {
					if (issue.severity == severity
						&& issue.shaderUid == shaderUid
						&& issue.pipelineUid == pipelineUid
						&& issue.sguid == sguid
						&& ReShapeNormalizeIssueMessageForDeduplication(issue.message) == normalizedMessage) {
						ReShapeAppendRollingExecutionUid(issue.rollingExecutionUids, rollingExecutionUid);
						break;
					}
				}
			}
		}
	}
}