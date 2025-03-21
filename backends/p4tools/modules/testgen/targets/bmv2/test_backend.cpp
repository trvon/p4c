#include "backends/p4tools/modules/testgen/targets/bmv2/test_backend.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

#include <boost/multiprecision/cpp_int.hpp>

#include "backends/p4tools/common/lib/model.h"
#include "backends/p4tools/common/lib/trace_event.h"
#include "backends/p4tools/common/lib/util.h"
#include "ir/ir.h"
#include "ir/irutils.h"
#include "lib/cstring.h"
#include "lib/error.h"
#include "lib/exceptions.h"

#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"
#include "backends/p4tools/modules/testgen/lib/test_backend.h"
#include "backends/p4tools/modules/testgen/lib/test_object.h"
#include "backends/p4tools/modules/testgen/options.h"
#include "backends/p4tools/modules/testgen/targets/bmv2/program_info.h"
#include "backends/p4tools/modules/testgen/targets/bmv2/test_backend/metadata.h"
#include "backends/p4tools/modules/testgen/targets/bmv2/test_backend/protobuf.h"
#include "backends/p4tools/modules/testgen/targets/bmv2/test_backend/protobuf_ir.h"
#include "backends/p4tools/modules/testgen/targets/bmv2/test_backend/ptf.h"
#include "backends/p4tools/modules/testgen/targets/bmv2/test_backend/stf.h"
#include "backends/p4tools/modules/testgen/targets/bmv2/test_spec.h"

namespace P4::P4Tools::P4Testgen::Bmv2 {

const std::set<std::string> Bmv2TestBackend::SUPPORTED_BACKENDS = {"PTF", "STF", "PROTOBUF",
                                                                   "PROTOBUF_IR", "METADATA"};

Bmv2TestBackend::Bmv2TestBackend(const Bmv2V1ModelProgramInfo &programInfo,
                                 const TestBackendConfiguration &testBackendConfiguration,
                                 SymbolicExecutor &symbex)
    : TestBackEnd(programInfo, testBackendConfiguration, symbex) {
    cstring testBackendString = TestgenOptions::get().testBackend;
    if (testBackendString.isNullOrEmpty()) {
        error(
            "No test back end provided. Please provide a test back end using the --test-backend "
            "parameter. Supported back ends are %1%.",
            Utils::containerToString(SUPPORTED_BACKENDS));
        exit(EXIT_FAILURE);
    }

    if (testBackendString == "PTF") {
        testWriter = new PTF(testBackendConfiguration);
    } else if (testBackendString == "STF") {
        testWriter = new STF(testBackendConfiguration);
    } else if (testBackendString == "PROTOBUF") {
        testWriter = new Protobuf(testBackendConfiguration, programInfo.getP4RuntimeAPI());
    } else if (testBackendString == "PROTOBUF_IR") {
        testWriter = new ProtobufIr(testBackendConfiguration, programInfo.getP4RuntimeAPI());
    } else if (testBackendString == "METADATA") {
        testWriter = new Metadata(testBackendConfiguration);
    } else {
        P4C_UNIMPLEMENTED(
            "Test back end %1% not implemented for this target. Supported back ends are %2%.",
            testBackendString, Utils::containerToString(SUPPORTED_BACKENDS));
    }
}

TestBackEnd::TestInfo Bmv2TestBackend::produceTestInfo(
    const ExecutionState *executionState, const Model *finalModel,
    const IR::Expression *outputPacketExpr, const IR::Expression *outputPortExpr,
    const std::vector<std::reference_wrapper<const TraceEvent>> *programTraces) {
    auto testInfo = TestBackEnd::produceTestInfo(executionState, finalModel, outputPacketExpr,
                                                 outputPortExpr, programTraces);
    return testInfo;
}

const TestSpec *Bmv2TestBackend::createTestSpec(const ExecutionState *executionState,
                                                const Model *finalModel, const TestInfo &testInfo) {
    const auto *ingressPayload = testInfo.inputPacket;
    const auto *ingressPayloadMask = IR::Constant::get(IR::Type_Bits::get(1), 1);
    const auto ingressPacket = Packet(testInfo.inputPort, ingressPayload, ingressPayloadMask);

    std::optional<Packet> egressPacket = std::nullopt;
    if (!testInfo.packetIsDropped) {
        egressPacket = Packet(testInfo.outputPort, testInfo.outputPacket, testInfo.packetTaintMask);
    }
    // Create a testSpec.
    auto *testSpec = new TestSpec(ingressPacket, egressPacket, testInfo.programTraces);

    // If metadata mode is enabled, gather the user metadata variable form the parser.
    // Save the values of all the fields in it and return.
    if (TestgenOptions::get().testBackend == "METADATA") {
        auto *metadataCollection = new MetadataCollection();
        const auto *bmv2ProgInfo = getProgramInfo().checkedTo<Bmv2V1ModelProgramInfo>();
        const auto *localMetadataVar = bmv2ProgInfo->getBlockParam("Parser"_cs, 2);
        const auto &flatFields = executionState->getFlatFields(localMetadataVar, {});
        for (const auto &fieldRef : flatFields) {
            const auto *fieldVal = finalModel->evaluate(executionState->get(fieldRef), true);
            // Try to remove the leading internal name for the metadata field.
            // Thankfully, this string manipulation is safe if we are out of range.
            auto fieldString = fieldRef->toString();
            fieldString = fieldString.substr(fieldString.find('.') - fieldString.begin() + 1);
            metadataCollection->addMetaDataField(fieldString, fieldVal);
        }
        testSpec->addTestObject("metadata_collection"_cs, "metadata_collection"_cs,
                                metadataCollection);
        return testSpec;
    }

    // We retrieve the individual table configurations from the execution state.
    const auto uninterpretedTableConfigs = executionState->getTestObjectCategory("tableconfigs"_cs);
    // Since these configurations are uninterpreted we need to convert them. We launch a
    // helper function to solve the variables involved in each table configuration.
    for (const auto &tablePair : uninterpretedTableConfigs) {
        const auto tableName = tablePair.first;
        const auto *uninterpretedTableConfig = tablePair.second->checkedTo<TableConfig>();
        const auto *tableConfig = uninterpretedTableConfig->evaluate(*finalModel, true);
        testSpec->addTestObject("tables"_cs, tableName, tableConfig);
    }

    const auto actionProfiles = executionState->getTestObjectCategory("action_profile"_cs);
    for (const auto &testObject : actionProfiles) {
        const auto profileName = testObject.first;
        const auto *actionProfile = testObject.second->checkedTo<Bmv2V1ModelActionProfile>();
        const auto *evaluatedProfile = actionProfile->evaluate(*finalModel, true);
        testSpec->addTestObject("action_profiles"_cs, profileName, evaluatedProfile);
    }

    const auto actionSelectors = executionState->getTestObjectCategory("action_selector"_cs);
    for (const auto &testObject : actionSelectors) {
        const auto selectorName = testObject.first;
        const auto *actionSelector = testObject.second->checkedTo<Bmv2V1ModelActionSelector>();
        const auto *evaluatedSelector = actionSelector->evaluate(*finalModel, true);
        testSpec->addTestObject("action_selectors"_cs, selectorName, evaluatedSelector);
    }

    const auto cloneSpecs = executionState->getTestObjectCategory("clone_specs"_cs);
    for (const auto &testObject : cloneSpecs) {
        const auto sessionId = testObject.first;
        const auto *cloneSpec = testObject.second->checkedTo<Bmv2V1ModelCloneSpec>();
        const auto *evaluatedInfo = cloneSpec->evaluate(*finalModel, true);
        testSpec->addTestObject("clone_specs"_cs, sessionId, evaluatedInfo);
    }

    const auto meterInfos = executionState->getTestObjectCategory("meter_values"_cs);
    for (const auto &testObject : meterInfos) {
        const auto meterName = testObject.first;
        const auto *meterInfo = testObject.second->checkedTo<Bmv2V1ModelMeterValue>();
        const auto *evaluateMeterValue = meterInfo->evaluate(*finalModel, true);
        testSpec->addTestObject("meter_values"_cs, meterName, evaluateMeterValue);
    }

    return testSpec;
}

}  // namespace P4::P4Tools::P4Testgen::Bmv2
