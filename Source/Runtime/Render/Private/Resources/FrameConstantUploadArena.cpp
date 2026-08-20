#include "Resources/FrameConstantUploadArena.h"

#include "Core/Log.h"

#include <cstring>
#include <limits>

namespace RVX
{
namespace
{
    constexpr uint64 RVX_MAX_DYNAMIC_CONSTANT_OFFSET =
        static_cast<uint64>(std::numeric_limits<uint32>::max());

    // Frame constants are consumed only by the graphics submission currently
    // being recorded. A completed point from another queue, or an older
    // graphics point, could make the page reusable while this work is still
    // executing. Require precisely one current graphics point and reject
    // terminally lost evidence before publishing the page as in flight.
    bool IsExactCurrentGraphicsSubmissionToken(
        const RenderSubmissionTracker& tracker,
        const GPUCompletionToken& token) noexcept
    {
        if (token.count != 1)
        {
            return false;
        }

        const GPUCompletionPoint point = token.points[0];
        return point.domain == GPUQueueDomain::Graphics &&
               point.value != 0 &&
               point.value == tracker.GetLastSubmittedValue(
                   GPUQueueDomain::Graphics) &&
               tracker.Query(token) != GPUCompletionStatus::Lost;
    }
}

bool FrameConstantUploadArena::Initialize(IRHIDevice* device,
                                          uint64 stride,
                                          uint32 slotsPerPage,
                                          std::string debugName)
{
    Shutdown();
    if (device == nullptr || stride == 0 || slotsPerPage == 0 ||
        stride > RVX_MAX_DYNAMIC_CONSTANT_OFFSET ||
        (slotsPerPage > 1 &&
         stride > RVX_MAX_DYNAMIC_CONSTANT_OFFSET / (slotsPerPage - 1)) ||
        stride > std::numeric_limits<uint64>::max() / slotsPerPage)
    {
        RVX_RENDER_ERROR("FrameConstantUploadArena: invalid page configuration");
        return false;
    }

    m_device = device;
    m_stride = stride;
    m_slotsPerPage = slotsPerPage;
    m_debugName = std::move(debugName);
    return true;
}

void FrameConstantUploadArena::Shutdown() noexcept
{
    m_pages.clear();
    m_debugName.clear();
    m_nextPageIdentity = 1;
    m_slotsPerPage = 0;
    m_stride = 0;
    m_tracker = nullptr;
    m_device = nullptr;
}

bool FrameConstantUploadArena::IsCompletionSatisfied(GPUCompletionStatus status) noexcept
{
    return status == GPUCompletionStatus::Completed ||
           status == GPUCompletionStatus::CompatibilityWaitIdle;
}

bool FrameConstantUploadArena::IsTokenUsable(const GPUCompletionToken& token) noexcept
{
    if (token.count == 0 || token.count > token.points.size())
    {
        return false;
    }
    for (uint8 index = 0; index < token.count; ++index)
    {
        const GPUCompletionPoint point = token.points[index];
        if (!IsDeclaredGPUQueueDomain(point.domain) || point.value == 0 ||
            (index != 0 && token.points[index - 1].domain >= point.domain))
        {
            return false;
        }
    }
    return true;
}

void FrameConstantUploadArena::PollCompletions() noexcept
{
    for (Page& page : m_pages)
    {
        if (page.state != PageState::InFlight)
        {
            continue;
        }
        if (m_tracker == nullptr || !IsTokenUsable(page.completion))
        {
            page.state = PageState::Unusable;
            continue;
        }
        const GPUCompletionStatus status = m_tracker->Query(page.completion);
        if (IsCompletionSatisfied(status))
        {
            page.completion = {};
            page.cursor = 0;
            page.state = PageState::Free;
        }
        else if (status == GPUCompletionStatus::Lost)
        {
            page.state = PageState::Unusable;
        }
    }
}

bool FrameConstantUploadArena::CreatePage(Page& outPage)
{
    if (m_device == nullptr || m_stride == 0 || m_slotsPerPage == 0 ||
        m_stride > std::numeric_limits<uint64>::max() / m_slotsPerPage)
    {
        return false;
    }
    RHIBufferDesc desc;
    desc.size = m_stride * m_slotsPerPage;
    desc.usage = RHIBufferUsage::Constant;
    desc.memoryType = RHIMemoryType::Upload;
    desc.debugName = m_debugName.c_str();
    RHIBufferRef buffer = m_device->CreateBuffer(desc);
    if (!buffer)
    {
        RVX_RENDER_ERROR("FrameConstantUploadArena: failed to create '{}'", desc.debugName);
        return false;
    }
    outPage = {};
    outPage.buffer = std::move(buffer);
    outPage.identity = m_nextPageIdentity++;
    outPage.state = PageState::Recording;
    return true;
}

FrameConstantUploadArena::Page* FrameConstantUploadArena::AcquireRecordingPage()
{
    PollCompletions();
    for (Page& page : m_pages)
    {
        if (page.state == PageState::Recording && page.cursor < m_slotsPerPage)
        {
            return &page;
        }
        if (page.state == PageState::Free)
        {
            page.state = PageState::Recording;
            page.cursor = 0;
            return &page;
        }
    }
    Page page;
    if (!CreatePage(page))
    {
        return nullptr;
    }
    m_pages.emplace_back(std::move(page));
    return &m_pages.back();
}

bool FrameConstantUploadArena::Allocate(const void* source,
                                        uint64 sourceSize,
                                        FrameConstantUploadAllocation& outAllocation)
{
    outAllocation = {};
    if (source == nullptr || sourceSize == 0 || sourceSize > m_stride ||
        !m_device || m_device->QueryRuntimeStatus() != RHIDeviceRuntimeStatus::Ready)
    {
        return false;
    }
    Page* page = AcquireRecordingPage();
    if (page == nullptr || !page->buffer || page->cursor >= m_slotsPerPage)
    {
        return false;
    }
    const uint64 offset = static_cast<uint64>(page->cursor) * m_stride;
    if (offset > RVX_MAX_DYNAMIC_CONSTANT_OFFSET)
    {
        page->state = PageState::Unusable;
        return false;
    }
    void* mapped = page->buffer->Map();
    if (mapped == nullptr)
    {
        page->state = PageState::Unusable;
        return false;
    }
    std::memset(static_cast<uint8*>(mapped) + offset, 0, static_cast<size_t>(m_stride));
    std::memcpy(static_cast<uint8*>(mapped) + offset, source, static_cast<size_t>(sourceSize));
    if (!page->buffer->CommitMappedWrite())
    {
        // A failed host-write commit gives us no trustworthy contents for this
        // page.  In particular, it may have left an earlier successful slot
        // resident while the current slot is not visible to the GPU.  Do not
        // advance the cursor or expose an allocation that aliases that state;
        // retire the page from further recording and let the next request use
        // a newly created page.
        page->state = PageState::Unusable;
        return false;
    }

    outAllocation.buffer = page->buffer;
    outAllocation.pageIdentity = page->identity;
    outAllocation.slot = page->cursor++;
    outAllocation.dynamicOffset = static_cast<uint32>(offset);
    return true;
}

bool FrameConstantUploadArena::NotifySubmission(const GPUCompletionToken& completion) noexcept
{
    const bool valid = m_tracker != nullptr &&
        IsTokenUsable(completion) &&
        IsExactCurrentGraphicsSubmissionToken(*m_tracker, completion);
    bool succeeded = valid;
    for (Page& page : m_pages)
    {
        if (page.state != PageState::Recording)
        {
            continue;
        }
        if (!valid)
        {
            page.state = PageState::Unusable;
            succeeded = false;
            continue;
        }
        page.completion = completion;
        page.state = PageState::InFlight;
    }
    return succeeded;
}

void FrameConstantUploadArena::ReleaseUnsubmittedFrame() noexcept
{
    for (Page& page : m_pages)
    {
        if (page.state == PageState::Recording)
        {
            page.completion = {};
            page.cursor = 0;
            page.state = PageState::Free;
        }
    }
}

uint32 FrameConstantUploadArena::GetUnusablePageCount() const noexcept
{
    uint32 count = 0;
    for (const Page& page : m_pages)
    {
        count += page.state == PageState::Unusable ? 1u : 0u;
    }
    return count;
}
} // namespace RVX
