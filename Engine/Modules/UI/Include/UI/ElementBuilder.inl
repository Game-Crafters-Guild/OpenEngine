#pragma once

namespace GameEngine::UI
{
template<class T> requires std::derived_from<T, UIElement>
ElementBuilder<T>::ElementBuilder(std::string_view tag)
    : m_Element(UIRegistration::ElementFactoryRegistry::Instance().Create<T>(tag))
{
    if (m_Element)
        UIRegistration::ElementFactoryRegistry::Instance().ApplyAttributes(*m_Element, {}, {});
}

template<class T> requires std::derived_from<T, UIElement>
ElementBuilder<T>& ElementBuilder<T>::Id(std::string_view id) &
{
    if (m_Element)
        m_Element->SetId(std::string(id));
    return *this;
}

template<class T> requires std::derived_from<T, UIElement>
ElementBuilder<T>&& ElementBuilder<T>::Id(std::string_view id) &&
{
    Id(id);
    return std::move(*this);
}

template<class T> requires std::derived_from<T, UIElement>
ElementBuilder<T>& ElementBuilder<T>::Id(LiteralName id) &
{
    if (m_Element)
        m_Element->SetId(id);
    return *this;
}

template<class T> requires std::derived_from<T, UIElement>
ElementBuilder<T>&& ElementBuilder<T>::Id(LiteralName id) &&
{
    Id(id);
    return std::move(*this);
}

template<class T> requires std::derived_from<T, UIElement>
template<Detail::ElementName Name, Detail::ElementName... ClassNames>
ElementBuilder<T>& ElementBuilder<T>::Class(Name&& className, ClassNames&&... classNames) &
{
    if (m_Element)
    {
        ApplyClass(std::forward<Name>(className));
        (ApplyClass(std::forward<ClassNames>(classNames)), ...);
    }
    return *this;
}

template<class T> requires std::derived_from<T, UIElement>
template<Detail::ElementName Name, Detail::ElementName... ClassNames>
ElementBuilder<T>&& ElementBuilder<T>::Class(Name&& className, ClassNames&&... classNames) &&
{
    Class(std::forward<Name>(className), std::forward<ClassNames>(classNames)...);
    return std::move(*this);
}

template<class T> requires std::derived_from<T, UIElement>
template<Detail::ElementName Name>
void ElementBuilder<T>::ApplyClass(Name&& className)
{
    if constexpr (std::same_as<std::remove_cvref_t<Name>, LiteralName>)
        m_Element->AddClass(className);
    else
        m_Element->AddClass(std::string(std::string_view(std::forward<Name>(className))));
}

template<class T> requires std::derived_from<T, UIElement>
template<class Setter, class... Arguments> requires Detail::ElementSetter<Setter, T, Arguments...>
ElementBuilder<T>& ElementBuilder<T>::Set(Setter setter, Arguments&&... arguments) &
{
    if (m_Element)
        std::invoke(setter, *m_Element, std::forward<Arguments>(arguments)...);
    return *this;
}

template<class T> requires std::derived_from<T, UIElement>
template<class Setter, class... Arguments> requires Detail::ElementSetter<Setter, T, Arguments...>
ElementBuilder<T>&& ElementBuilder<T>::Set(Setter setter, Arguments&&... arguments) &&
{
    Set(setter, std::forward<Arguments>(arguments)...);
    return std::move(*this);
}

template<class T> requires std::derived_from<T, UIElement>
template<class Setter, class Value, class... Remaining>
    requires (sizeof...(Remaining) >= 2) &&
             Detail::ElementSetterPairs<T, Setter, Value, Remaining...>::value &&
             (!Detail::ElementSetter<Setter, T, Value, Remaining...>)
ElementBuilder<T>& ElementBuilder<T>::Set(Setter setter, Value&& value, Remaining&&... remaining) &
{
    Set(setter, std::forward<Value>(value));
    Set(std::forward<Remaining>(remaining)...);
    return *this;
}

template<class T> requires std::derived_from<T, UIElement>
template<class Setter, class Value, class... Remaining>
    requires (sizeof...(Remaining) >= 2) &&
             Detail::ElementSetterPairs<T, Setter, Value, Remaining...>::value &&
             (!Detail::ElementSetter<Setter, T, Value, Remaining...>)
ElementBuilder<T>&& ElementBuilder<T>::Set(Setter setter, Value&& value, Remaining&&... remaining) &&
{
    Set(setter, std::forward<Value>(value), std::forward<Remaining>(remaining)...);
    return std::move(*this);
}

template<class T> requires std::derived_from<T, UIElement>
template<class... ChildrenTypes> requires (Detail::OwnedElementChild<ChildrenTypes> && ...)
ElementBuilder<T>& ElementBuilder<T>::Children(ChildrenTypes&&... children) &
{
    (Append(std::forward<ChildrenTypes>(children)), ...);
    return *this;
}

template<class T> requires std::derived_from<T, UIElement>
template<class... ChildrenTypes> requires (Detail::OwnedElementChild<ChildrenTypes> && ...)
ElementBuilder<T>&& ElementBuilder<T>::Children(ChildrenTypes&&... children) &&
{
    Children(std::forward<ChildrenTypes>(children)...);
    return std::move(*this);
}

template<class T> requires std::derived_from<T, UIElement>
ElementBuilder<T>& ElementBuilder<T>::On(EventId id, UIElement::EventHandler handler) &
{
    if (m_Element)
        m_Element->RegisterEventHandler(id, std::move(handler));
    return *this;
}

template<class T> requires std::derived_from<T, UIElement>
ElementBuilder<T>&& ElementBuilder<T>::On(EventId id, UIElement::EventHandler handler) &&
{
    On(id, std::move(handler));
    return std::move(*this);
}

template<class T> requires std::derived_from<T, UIElement>
ElementBuilder<T>& ElementBuilder<T>::Capture(UIElement::WeakRef<T>& reference) &
{
    reference = UIElement::MakeWeakRef(m_Element.get());
    return *this;
}

template<class T> requires std::derived_from<T, UIElement>
ElementBuilder<T>&& ElementBuilder<T>::Capture(UIElement::WeakRef<T>& reference) &&
{
    Capture(reference);
    return std::move(*this);
}

template<class T> requires std::derived_from<T, UIElement>
std::unique_ptr<T> ElementBuilder<T>::Build() &&
{
    return std::move(m_Element);
}

template<class T> requires std::derived_from<T, UIElement>
template<class Child>
void ElementBuilder<T>::Append(Child&& child)
{
    if constexpr (requires { std::forward<Child>(child).Build(); })
    {
        Append(std::forward<Child>(child).Build());
    }
    else
    {
        // Take ownership even after a preceding child failed, so a named unique_ptr
        // is consumed just like a builder and every captured weak reference expires.
        std::unique_ptr<UIElement> ownedChild = std::forward<Child>(child);
        if (!m_Element || !ownedChild)
        {
            m_Element.reset();
            return;
        }
        m_Element->AddChild(std::move(ownedChild));
    }
}

template<class T> requires std::derived_from<T, UIElement>
ElementBuilder<T> Element(std::string_view tag)
{
    return ElementBuilder<T>(tag);
}
}
