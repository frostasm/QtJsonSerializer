#include "qjsongadgetconverter_p.h"
#include "qjsonserializerexception.h"
#include "qjsonserializer_p.h"

#include <QtCore/QMetaProperty>
#include <QtCore/QSet>

bool QJsonGadgetConverter::canConvert(int metaTypeId) const
{
	// exclude a few Qt gadgets that have no properties and thus need to be handled otherwise
	static const QSet<int> gadgetExceptions {
		QMetaType::QKeySequence,
		QMetaType::QFont,
		QMetaType::QLocale,
	};
	if(gadgetExceptions.contains(metaTypeId))
		return false;

	const auto flags = QMetaType::typeFlags(metaTypeId);
	return flags.testFlag(QMetaType::IsGadget) ||
			flags.testFlag(QMetaType::PointerToGadget);
}

QList<QJsonValue::Type> QJsonGadgetConverter::jsonTypes() const
{
	return {QJsonValue::Object, QJsonValue::Null};
}

QJsonValue QJsonGadgetConverter::serialize(int propertyType, const QVariant &value, const QJsonTypeConverter::SerializationHelper *helper) const
{
	const auto metaObject = QMetaType::metaObjectForType(propertyType);
	if(!metaObject)
		throw QJsonSerializationException(QByteArray("Unable to get metaobject for type ") + QMetaType::typeName(propertyType));
	const auto isPtr = QMetaType::typeFlags(propertyType).testFlag(QMetaType::PointerToGadget);

	auto gValue = value;
	if(!gValue.convert(propertyType))
		throw QJsonSerializationException(QByteArray("Data is not of the required gadget type ") + QMetaType::typeName(propertyType));
	const void *gadget = nullptr;
	if(isPtr) {
		// with pointers, null gadgets are allowed
		gadget = *reinterpret_cast<const void* const *>(gValue.constData());
		if(!gadget)
			return QJsonValue::Null;
	} else
		gadget = gValue.constData();
	if(!gadget)
		throw QJsonSerializationException(QByteArray("Unable to get address of gadget ") + QMetaType::typeName(propertyType));

	QJsonObject jsonObject;
	//go through all properties and try to serialize them
	for(auto i = 0; i < metaObject->propertyCount(); i++) {
		auto property = metaObject->property(i);
		if(property.isStored())
			jsonObject[QString::fromUtf8(property.name())] = helper->serializeSubtype(property, property.readOnGadget(gadget));
	}

	const bool serializeClassInfo = helper->getProperty("serializeClassInfo").toBool();
	const int classInfoCount = metaObject->classInfoCount();
	if (serializeClassInfo && classInfoCount > 0) {
		const QString prefix = helper->getProperty("classInfoKeyPrefix").toString();
		const QString suffix = helper->getProperty("classInfoKeySuffix").toString();
		for(int i = 0; i < classInfoCount; ++i) {
			const QMetaClassInfo clInfo = metaObject->classInfo(i);
			const QString key = prefix + QString::fromUtf8(clInfo.name()) + suffix;
			if (jsonObject.contains(key)) {
				const QString error = QStringLiteral("classInfo key name \"%1\" override property of gadget class %2")
						.arg(key).arg(QLatin1Literal(QMetaType::typeName(propertyType)));
				throw QJsonSerializationException(error.toUtf8());
			}

			jsonObject[key] = QString::fromUtf8(clInfo.value());
		}
	}


	return jsonObject;
}

QVariant QJsonGadgetConverter::deserialize(int propertyType, const QJsonValue &value, QObject *parent, const QJsonTypeConverter::SerializationHelper *helper) const
{
	Q_UNUSED(parent)//gadgets neither have nor serve as parent
	const auto isPtr = QMetaType::typeFlags(propertyType).testFlag(QMetaType::PointerToGadget);

	auto metaObject = QMetaType::metaObjectForType(propertyType);
	if(!metaObject)
		throw QJsonDeserializationException(QByteArray("Unable to get metaobject for gadget type") + QMetaType::typeName(propertyType));

	QVariant gadget;
	void *gadgetPtr = nullptr;
	if(isPtr) {
		if(value.isNull())
			return QVariant{propertyType, nullptr}; //initialize an empty (nullptr) variant
		const auto gadgetType = QMetaType::type(metaObject->className());
		if(gadgetType == QMetaType::UnknownType)
			throw QJsonDeserializationException(QByteArray("Unable to get type of gadget from gadget-pointer type") + QMetaType::typeName(propertyType));
		gadgetPtr = QMetaType::create(gadgetType);
		gadget = QVariant{propertyType, &gadgetPtr};
	} else {
		if(value.isNull())
			return QVariant{}; //will trigger a fail next stage as nullptr is not convertible to a gadget
		gadget = QVariant{propertyType, nullptr};
		gadgetPtr = gadget.data();
	}

	if(!gadgetPtr) {
		throw QJsonDeserializationException(QByteArray("Failed to construct gadget of type ") +
											QMetaType::typeName(propertyType) +
											QByteArray(". Does is have a default constructor?"));
	}

	auto jsonObject = value.toObject();
	auto validationFlags = helper->getProperty("validationFlags").value<QJsonSerializer::ValidationFlags>();

	//collect required properties, if set
	QSet<QByteArray> reqProps;
	if(validationFlags.testFlag(QJsonSerializer::AllProperties)) {
		for(auto i = 0; i < metaObject->propertyCount(); i++) {
			auto property = metaObject->property(i);
			if(property.isStored())
				reqProps.insert(property.name());
		}
	}

	//now deserialize all json properties
	for(auto it = jsonObject.constBegin(); it != jsonObject.constEnd(); it++) {
		auto propIndex = metaObject->indexOfProperty(qUtf8Printable(it.key()));
		if(propIndex != -1) {
			auto property = metaObject->property(propIndex);
			auto subValue = helper->deserializeSubtype(property, it.value(), nullptr);
			property.writeOnGadget(gadgetPtr, subValue);
			reqProps.remove(property.name());
		} else if(validationFlags.testFlag(QJsonSerializer::NoExtraProperties)) {
			throw QJsonDeserializationException("Found extra property " +
												it.key().toUtf8() +
												" but extra properties are not allowed");
		}
	}

	//make shure all required properties have been read
	if(validationFlags.testFlag(QJsonSerializer::AllProperties) && !reqProps.isEmpty()) {
		throw QJsonDeserializationException(QByteArray("Not all properties for ") +
											metaObject->className() +
											QByteArray(" are present in the json object. Missing properties: ") +
											reqProps.toList().join(", "));
	}

	return gadget;
}
