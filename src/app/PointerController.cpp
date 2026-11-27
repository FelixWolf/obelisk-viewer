#include "PointerController.hpp"

#include "viewer/core/Log.hpp"

namespace viewer
{

namespace
{

protocol::Vector3 toVector(const glm::vec3& v)
{
    return {v.x, v.y, v.z};
}

} // namespace

void PointerController::aim(render::Scene& scene, const std::optional<Ray>& ray)
{
    aimed_ = ray.has_value();
    if(!ray)
    {
        scene.pick.reset();
        hover_.reset();
        transparent_.reset();
        press_.reset();
        return;
    }
    render::PickRequest request;
    request.id = nextRequest_++;
    request.origin = ray->origin;
    request.direction = glm::normalize(ray->direction);
    request.hudPoint = ray->hudPoint;
    scene.pick = request;
    latestRequest_ = request.id;
}

void PointerController::update(render::Renderer& renderer, const world::World& world,
                               const ObjectView& objects)
{
    const auto result = renderer.takePick();
    if(!result || !aimed_)
    {
        return;
    }
    auto hoverOf = [&](const render::PickHit& hit) -> std::optional<Hover>
    {
        const auto picked = objects.resolvePick(hit.pickId);
        const world::Object* object =
            picked ? world.object(picked->key.region, picked->key.localId) : nullptr;
        if(!object)
        {
            return std::nullopt;
        }
        const world::Object& root = ObjectView::linkRoot(*object, world);
        Hover hover;
        hover.key = picked->key;
        hover.face = picked->face;
        hover.hud = hit.hud;
        hover.touchable = ObjectView::touchable(*object, root);
        hover.clickAction = ObjectView::clickAction(*object, root);
        hover.attachment = root.parentId != 0; // its root sits on an avatar
        hover.hit = hit;
        return hover;
    };
    hover_ = hoverOf(*result);
    transparent_ = hoverOf(result->transparent);

    // What a press would touch, as the official left click chooses.
    const auto& t = transparent_;
    const auto& v = hover_;
    auto actionable = [](const Hover& h)
    { return h.touchable || h.clickAction != world::ClickAction::Touch; };
    if(!t || !v || t->key == v->key || t->attachment || actionable(*t))
    {
        press_ = t ? t : v;
    }
    else if(v->attachment || actionable(*v))
    {
        press_ = v;
    }
    else
    {
        press_ = t;
    }
}

void PointerController::press(session::Session& session, const world::World& world,
                              const ObjectView& objects, const AvatarView* avatars,
                              uint64_t originHandle, uint64_t agentRegion)
{
    release(session);
    if(!press_)
    {
        return;
    }
    if(press_->clickAction != world::ClickAction::Touch &&
       press_->clickAction != world::ClickAction::Sit)
    {
        // Pay, buy, open, play, zoom: not yet; a touch meanwhile. (Sit is
        // the app's: here only when it can't sit, seated already.)
        log::info("click action {} isn't supported yet: touching instead", press_->clickAction);
    }
    grab(session, world, objects, avatars, originHandle, agentRegion, *press_);
}

void PointerController::touch(session::Session& session, const world::World& world,
                              const ObjectView& objects, const AvatarView* avatars,
                              uint64_t originHandle, uint64_t agentRegion, const Hover& target)
{
    release(session);
    grab(session, world, objects, avatars, originHandle, agentRegion, target);
    release(session);
}

void PointerController::grab(session::Session& session, const world::World& world,
                             const ObjectView& objects, const AvatarView* avatars,
                             uint64_t originHandle, uint64_t agentRegion, const Hover& target)
{
    if(!target.touchable)
    {
        return;
    }
    const world::Object* object = world.object(target.key.region, target.key.localId);
    if(!object)
    {
        return;
    }
    // From the prim to its root, in the root's frame (lltoolgrab.cpp).
    const world::Object& root = ObjectView::linkRoot(*object, world);
    const auto frame = objects.objectFrame(*object, world, originHandle, avatars);
    const auto rootFrame = objects.objectFrame(root, world, originHandle, avatars);
    glm::vec3 grabOffset(0.0f);
    if(frame && rootFrame)
    {
        const glm::mat3 rootRotation(*rootFrame);
        grabOffset =
            glm::transpose(rootRotation) * (glm::vec3((*rootFrame)[3]) - glm::vec3((*frame)[3]));
    }
    const session::Session::TouchSurface surface = surfaceOf(target, originHandle, agentRegion);
    session.touchObject(object->region, object->localId, toVector(grabOffset), surface);
    grabbed_ = Grab{object->region,       object->localId,  target.key, object->id,
                    toVector(grabOffset), surface.position, surface,    0};
    log::info("touched {} (face {}{}): uv {:.3f} {:.3f}, st {:.3f} {:.3f}, at {:.3f} {:.3f} {:.3f}",
              object->id.toString(), target.face, target.hud ? ", HUD" : "", surface.uv.x,
              surface.uv.y, surface.st.x, surface.st.y, surface.position.x, surface.position.y,
              surface.position.z);
}

session::Session::TouchSurface
PointerController::surfaceOf(const Hover& hover, uint64_t originHandle, uint64_t agentRegion)
{
    const render::PickHit& hit = hover.hit;
    glm::vec3 position = hit.position;
    if(!hit.hud)
    {
        const glm::vec2 originCorner(static_cast<float>(originHandle >> 32),
                                     static_cast<float>(originHandle & 0xFFFFFFFFu));
        const glm::vec2 agentCorner(static_cast<float>(agentRegion >> 32),
                                    static_cast<float>(agentRegion & 0xFFFFFFFFu));
        position += glm::vec3(originCorner - agentCorner, 0.0f);
    }
    session::Session::TouchSurface surface;
    surface.uv = {hit.uv.x, hit.uv.y, 0.0f};
    surface.st = {hit.st.x, hit.st.y, 0.0f};
    surface.face = hover.face;
    surface.position = toVector(position);
    surface.normal = toVector(hit.normal);
    surface.binormal = toVector(hit.binormal);
    return surface;
}

void PointerController::drag(session::Session& session, uint64_t originHandle, uint64_t agentRegion,
                             uint64_t now)
{
    if(!grabbed_)
    {
        return;
    }
    if(grabbed_->sentAt == 0)
    {
        grabbed_->sentAt = now; // pressed this frame: the grab said where
        return;
    }
    // On the prim it holds: where; off it, nowhere on it (as the official
    // pick of that object alone finds no surface).
    session::Session::TouchSurface surface;
    // (Through the transparent pick first: the grab came from it, mostly.)
    if(transparent_ && transparent_->key == grabbed_->key)
    {
        surface = surfaceOf(*transparent_, originHandle, agentRegion);
    }
    else if(hover_ && hover_->key == grabbed_->key)
    {
        surface = surfaceOf(*hover_, originHandle, agentRegion);
    }
    else
    {
        surface.uv = {-1.0f, -1.0f, 0.0f};
        surface.st = {-1.0f, -1.0f, 0.0f};
        surface.face = -1;
    }
    if(surface == grabbed_->surface)
    {
        return;
    }
    const auto sinceLast = static_cast<uint32_t>((now - grabbed_->sentAt) / 1'000'000u);
    session.dragObject(grabbed_->region, grabbed_->object, grabbed_->offset, grabbed_->position,
                       sinceLast, surface);
    grabbed_->surface = surface;
    grabbed_->sentAt = now;
}

void PointerController::release(session::Session& session)
{
    if(!grabbed_)
    {
        return;
    }
    session.releaseObject(grabbed_->region, grabbed_->localId, grabbed_->surface);
    grabbed_.reset();
}

} // namespace viewer
