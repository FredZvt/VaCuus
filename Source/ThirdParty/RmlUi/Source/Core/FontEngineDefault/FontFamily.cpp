#include "FontFamily.h"
#include "../../../Include/RmlUi/Core/ComputedValues.h"
#include "../../../Include/RmlUi/Core/Log.h" // VaCuus patch #13
#include "../../../Include/RmlUi/Core/Math.h"
#include "FontFace.h"
#include <limits.h>

namespace Rml {

FontFamily::FontFamily(const String& name) : name(name) {}

FontFamily::~FontFamily()
{
	// Multiple face entries may share memory within a single font family, although only one of them owns it. Here we make sure that all the face
	// destructors are run before all the memory is released. This way we don't leave any hanging references to invalidated memory.
	for (FontFaceEntry& entry : font_faces)
		entry.face.reset();
}

FontFaceHandleDefault* FontFamily::GetFaceHandle(Style::FontStyle style, Style::FontWeight weight, int size)
{
	int best_dist = INT_MAX;
	FontFace* matching_face = nullptr;
	for (size_t i = 0; i < font_faces.size(); i++)
	{
		FontFace* face = font_faces[i].face.get();

		if (face->GetStyle() == style)
		{
			const int dist = Math::Absolute((int)face->GetWeight() - (int)weight);
			if (dist == 0)
			{
				// Direct match for weight, break the loop early.
				matching_face = face;
				break;
			}
			else if (dist < best_dist)
			{
				// Best match so far for weight, store the face and dist.
				matching_face = face;
				best_dist = dist;
			}
		}
	}

	if (!matching_face)
		return nullptr;

	// VaCuus patch #13 (VENDORED_TAG.txt): the nearest weight is drawn silently otherwise -- a bold title in a family that
	// only has a regular face renders regular with nothing logged. Once per (style, weight), since this runs per element.
	const int miss_key = (int)style * 10000 + (int)weight;
	if (matching_face->GetWeight() != weight && reported_weight_misses.find(miss_key) == reported_weight_misses.end() && reported_weight_misses.insert(miss_key).second)
		Log::Message(Log::LT_WARNING, "Font family '%s' has no %s face of weight %d; drawing its weight %d face instead. Load the missing face with LoadFontFace.", name.c_str(), style == Style::FontStyle::Italic ? "italic" : "normal", (int)weight, (int)matching_face->GetWeight());

	return matching_face->GetHandle(size, true);
}

auto FontFamily::AddFace(FontFaceHandleFreetype ft_face, Style::FontStyle style, Style::FontWeight weight, UniquePtr<byte[]> face_memory)
	-> AddFaceResult
{
	for (auto& face : font_faces)
	{
		if (face.face->GetStyle() == style && face.face->GetWeight() == weight)
		{
			return {FontProvider::FontFaceLoadResult::Duplicate, nullptr};
		}
	}

	auto face = MakeUnique<FontFace>(ft_face, style, weight);

	AddFaceResult result{FontProvider::FontFaceLoadResult::Success, face.get()};

	font_faces.push_back(FontFaceEntry{std::move(face), std::move(face_memory)});

	return result;
}

void FontFamily::ReleaseFontResources()
{
	for (auto& entry : font_faces)
		entry.face->ReleaseFontResources();
}

} // namespace Rml
